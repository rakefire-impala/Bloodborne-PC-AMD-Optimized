// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: camera motion vectors for temporal upscaling (docs/upscaler.md). The scene constants
// give the camera of each frame; the previous one is kept here. BB_DEBUG_MOTION=1 blends the
// motion vectors as colors into the frame before it is copied to the display.

#pragma once

#include <array>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/temporal_history.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {
struct FrameGenerationSource;

class Instance;
class Scheduler;
class Runtime;
class ObjectMotion;

class CameraMotion {
public:
    CameraMotion(const Instance& instance, Scheduler& scheduler,
                 VideoCore::TextureCache& texture_cache, Runtime& runtime);
    ~CameraMotion();
    void InvalidateHistory() { history.Invalidate(); }
    [[nodiscard]] bool NeedsCamera() const noexcept { return history.NeedsCamera(); }
    /// The rasterizer passes scene constants: the camera is still needed, or BB_CAMERA_LOG lists
    /// every scene camera of the frame.
    /// Scene constants at guest `address` are read once: a pass's draws share one buffer (the
    /// reflection and main passes have their own), and reading the constant ring (GPU memory)
    /// on every G-buffer draw cost ~20% of the frame rate.
    [[nodiscard]] bool WantsConstants(VAddr address) const noexcept {
        return address != checked_address && (NeedsCamera() || !settled || camera_log);
    }
    [[nodiscard]] bool HistoryReset() const noexcept { return history.Reset(); }
    void SetObjectMotion(ObjectMotion* motion) noexcept {
        object_motion = motion;
    }
    /// This frame's object motion image (after RecordMotion), for the debug view; or null.
    vk::ImageView ObjectMotionView() const noexcept;
    vk::Image ObjectMotionImage(u32 width, u32 height) const noexcept;

    void SetJitter(std::array<float, 2> value) noexcept {
        previous_jitter = jitter;
        jitter = value;
    }

    [[nodiscard]] bool Enabled() const noexcept {
        return debug_overlay || for_upscaler;
    }

    /// Both cameras and the scene depth of the current frame are known.
    [[nodiscard]] bool Ready() const noexcept {
        return current.valid && previous.valid && depth_id;
    }
    [[nodiscard]] VideoCore::ImageId Depth() const noexcept {
        return depth_id;
    }
    /// The render size in the scene constants (the game's viewport; its targets may be
    /// allocated larger, e.g. 1916x1080 for a 1916x1078 scene), or zero before the first camera.
    [[nodiscard]] std::array<u32, 2> RenderSize() const noexcept {
        return render_size;
    }
    /// Vertical field of view and near/far planes of the current camera.
    [[nodiscard]] float VerticalFov() const noexcept;
    [[nodiscard]] float Near() const noexcept;
    [[nodiscard]] std::array<float,12> InverseView() const noexcept { return current.inv_view; }
    void FrameGenerationCamera(FrameGenerationSource& source) const;
    // Current projection, previous projection, and row Z of previous-view * inverse-view.
    [[nodiscard]] std::array<std::array<float, 4>, 3> TaaDepthParameters() const noexcept;

    /// Records the motion vector pass: `depth_view` (depth aspect, General layout) into
    /// `motion_view` (RG16F storage, General), pixels, previous minus current.
    void RecordMotion(vk::CommandBuffer cmdbuf, vk::ImageView depth_view, vk::ImageView motion_view,
                      u32 width, u32 height);

    /// A bound constant buffer of 864 bytes (from guest `address`): checks the scene constant
    /// signature. `source`: 'r' the draw's constant-ring snapshot, 'g' guest memory.
    void OnConstants(const float* data, char source, VAddr address);

    /// The G-buffer pass (5+ color targets): its depth is the scene depth.
    void OnGBufferPass(VideoCore::ImageId depth);

    /// The pass copying the finished frame (`frame`, the last target drawn) to the display: the
    /// frame's depth and camera are complete. Records the debug overlay, starts a new frame.
    void OnDisplayPass(VideoCore::ImageId frame);

private:
    void Overlay(VideoCore::ImageId frame);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    bool debug_overlay = false;
    bool for_upscaler = false;
    /// BB_CAMERA_LOG=1: one line per frame (camera source, reset, position, direction).
    bool camera_log = false;
    u32 log_frame = 0;
    u32 rejected_constants = 0;
    char captured_source = '-';
    std::string seen_cameras;
    vk::UniqueDescriptorSetLayout motion_desc_layout;
    vk::UniquePipelineLayout motion_pipeline_layout;
    vk::UniquePipeline motion_pipeline;

    ObjectMotion* object_motion = nullptr;
    Motion::CameraHistory history;
    Motion::CameraSample& current = history.current;
    Motion::CameraSample& previous = history.previous;
    std::array<float, 2> jitter{}, previous_jitter{};
    std::array<u32, 2> render_size{};
    /// This frame's main camera is known: it replaced or followed a reflection camera.
    bool settled = false;
    /// Guest address of the scene constants read last this frame (OnConstants).
    VAddr checked_address = 0;
    /// Height of the last reflecting plane, and this frame's skipped reflection camera.
    std::optional<float> mirror_plane;
    std::optional<Motion::CameraSample> skipped;
    VideoCore::ImageId depth_id{};

    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline overlay_pipeline;
    /// Debug: the last two finished frames (RGBA8 packed), for the reprojection check.
    std::unique_ptr<VideoCore::Buffer> frames[2];
    u32 frame_index = 0;
};

} // namespace Vulkan
