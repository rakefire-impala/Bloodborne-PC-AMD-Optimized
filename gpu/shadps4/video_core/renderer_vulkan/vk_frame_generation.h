// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaAllocator)

struct FfxVkFsr3_3_1_6FrameGenerationContext;
namespace Vulkan {

// Immutable camera values captured before the display pass advances camera history.
struct FrameGenerationSource {
    vk::ImageView depth_view;
    vk::Image motion_image;
    vk::ImageView motion_view;
    u32 render_width{}, render_height{};
    std::array<float, 2> jitter{};
    std::array<float, 12> inverse_view{};
    float near_plane{}, vertical_fov{}, frame_ms{16.666f};
    bool reset{}, valid{};
    std::array<float,12> view{}, previous_view{}, previous_inverse_view{};
    std::array<float,4> projection{}, previous_projection{};
    // bbport: the finished UI image and its copy from before the game drew the HUD
    // (TemporalUpscaler::CopyHudless), both ui_width x ui_height; null when not available.
    vk::ImageView ui_view, hudless_view;
    u32 ui_width{}, ui_height{};
};

class FrameGeneration {
public:
    // Submission callbacks keep resource retirement tied to the queue that
    // actually runs FI/OF, including when tested on a headless Vulkan device.
    struct Submissions {
        std::function<void()> wait_submitted;
        std::function<void(u64)> wait;
        std::function<bool(u64)> is_free;
    };
    struct Image {
        VmaAllocator allocator{};
        VmaAllocation allocation{};
        vk::Image image{};
        Image() = default;
        ~Image();
        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
        bool Create(VmaAllocator allocator, vk::Format format, u32 width, u32 height,
                    const std::vector<u32>& queue_families = {});
        operator vk::Image() const { return image; }
    };
    struct Snapshot {
        FrameGenerationSource source;
        u32 width{}, height{};
        bool hdr{}, sdr_unorm{}, nvidia_input_only{};
        bool srgb8{}; ///< bbport: color and generated frame are sRGB-encoded RGBA8 (UseCompactColor)
        Image color, depth, motion, generated, encoded;
        vk::UniqueImageView color_view, depth_view, motion_view, generated_view, encoded_view;
        // bbport: the HUD (frame_generation_hud.comp): its mask, and the presented frame whose
        // HUD pixels go into the generated frame. hud: both were captured this frame.
        Image hud_mask;
        vk::UniqueImageView hud_mask_view;
        vk::ImageView frame_view;
        bool hud{}, frame_decodes{};
        vk::Image PresentImage() const {
            return hdr || (sdr_unorm && !srgb8) ? vk::Image(encoded) : vk::Image(generated);
        }
    };
    // queue_families: more than one shares every image between those families (frame
    // generation on an async compute queue, its input and output on the graphics family).
    FrameGeneration(vk::PhysicalDevice physical, vk::Device device,
                    VmaAllocator allocator, Submissions submissions,
                    std::vector<u32> queue_families = {});
    ~FrameGeneration();
    // Draw queue: copy per-frame depth/motion and decode SDR sRGB or HDR10 PQ
    // frame into stable linear color. Snapshot ownership follows the final fence.
    void Capture(vk::CommandBuffer cmdbuf, const FrameGenerationSource& source,
                 vk::ImageView color_view, u32 width, u32 height, vk::Format format,
                 std::shared_ptr<Snapshot>& snapshot);
    // Draw queue, after Capture (bbport: was the present queue): record the real FI/OF
    // prepare and dispatch. False still permits a history-seeding dispatch, but suppresses
    // the first generated frame.
    bool Record(vk::CommandBuffer cmdbuf, const std::shared_ptr<Snapshot>& snapshot);
    // bbport: diagnostics (BB_FRAME_STATS): called between the prepare (optical flow) and
    // the interpolation dispatch with the command buffer being recorded.
    std::function<void(vk::CommandBuffer)> between_passes;
    void Submitted(u64 tick);
    /// bbport: FSR frame generation is on (Capture will take this frame's inputs).
    bool Wanted() const;
    // bbport: SDR sRGB swapchains: frame interpolation on sRGB-encoded RGBA8 color instead of
    // linear FP16, half the bytes for every color read and write of the optical flow and the
    // interpolation. The generated frame is then sRGB-encoded RGBA8, which the presenter
    // copies as it is (a blit into an sRGB image would encode it twice).
    void UseCompactColor(bool on) {
        compact_color = on;
    }
    void Invalidate();
    void Reset();
    /// bbport: VRAM nearly exhausted (Presenter's guard): frame generation steps aside and
    /// keeps its memory (freeing it at the budget blocks in the driver) until there is room.
    void SetVramPaused(bool paused) {
        vram_paused.store(paused, std::memory_order_relaxed);
    }
    bool VramPaused() const {
        return vram_paused.load(std::memory_order_relaxed);
    }
private:
    bool EnsureCopyPipeline();
    // bbport: the HUD passes (frame_generation_hud.comp) and their lookup table buffer.
    bool EnsureHudPipelines();
    void CaptureHud(vk::CommandBuffer cmdbuf, Snapshot& snapshot, vk::ImageView color_view,
                    bool decodes);
    void CompositeHud(vk::CommandBuffer cmdbuf, const Snapshot& snapshot);
    void RetireCompleted();
    const vk::PhysicalDevice physical;
    const vk::Device device;
    const VmaAllocator allocator;
    Submissions submissions;
    const std::vector<u32> queue_families;
    vk::UniqueDescriptorSetLayout copy_descriptors;
    vk::UniquePipelineLayout copy_layout;
    vk::UniquePipeline copy_pipeline;
    struct HudPass {
        vk::UniqueDescriptorSetLayout descriptors;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    std::array<HudPass, 4> hud_passes; ///< LUT, resolve, capture, composite
    vk::Buffer hud_lut{};
    // The composite pass's memory: the HUD's measured opacity per pixel, and the previous real
    // frame's scene and presented pixels (the HUD's transmission between two frames).
    struct HudHistory {
        Image opacity, scene, frame;
        vk::UniqueImageView opacity_view, scene_view, frame_view;
        u32 width{}, height{};
    };
    std::unique_ptr<HudHistory> hud_history;
    VmaAllocation hud_lut_allocation{};
    bool hud_failed{};
    FfxVkFsr3_3_1_6FrameGenerationContext* context{};
    u32 render_width{}, render_height{}, width{}, height{};
    u64 frame_id{}, paused_ns{};
    std::atomic<bool> history{}; ///< Invalidate() comes from the swap thread
    bool context_hdr{}, compact_color{};
    vk::Format context_format{};
    std::atomic<bool> failed{false};
    std::atomic<bool> vram_paused{false};
    struct Pending {
        u64 id{}, tick{};
        std::shared_ptr<Snapshot> snapshot;
    };
    std::deque<Pending> pending;
    Pending recording;
};
} // namespace Vulkan
