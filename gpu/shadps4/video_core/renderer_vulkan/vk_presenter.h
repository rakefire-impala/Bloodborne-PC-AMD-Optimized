// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include "bbport_amd.h"

#include <condition_variable>

#include "core/libraries/videoout/buffer.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};
    std::shared_ptr<FrameGeneration::Snapshot> frame_generation;
    void* nvidia_token{};
    BbAmd::FrameToken amd_token{};
    vk::Semaphore nvidia_fence{};
    u64 nvidia_fence_value{};
    u64 nvidia_fence_epoch{};
    std::chrono::steady_clock::time_point flip_time{}; ///< bbport: when the guest flip was released
    // bbport: frame generation, done in the draw stream (PrepareFrame): the generated frame in
    // the swapchain format, for a plain copy at presentation, and whether this frame has one.
    VmaAllocation generated_allocation{};
    vk::Image generated_image{};
    vk::ImageView generated_view{};      ///< in the swapchain format (the overlay draws into it)
    vk::Format generated_format{};       ///< of generated_image: the UNORM twin for 8-bit sRGB FI
    bool generated{};
    bool overlay_drawn{}; ///< the overlay was drawn into image (and generated_image) already
    bool compute_present{}; ///< presented by copies on the compute queue (PresentCompute)
};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);

    Frame* PrepareBlankFrame(bool present_thread);

    /// bbport: a guest flip of `frame` is queued for Present (the video out present thread).
    void FlipQueued(Frame* frame) {
        frame->flip_time = std::chrono::steady_clock::now();
    }

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    Frame* PrepareLastFrame();

private:
    Frame* GetRenderFrame();

    /// bbport: frame generation of a drawn frame, in the draw stream (PrepareFrame).
    void Generate(Frame* frame, vk::CommandBuffer cmdbuf);
    /// bbport: copies `source` into the acquired swapchain image on the compute queue and
    /// presents it, not before `present_at` when given (the copy is submitted at once).
    /// Returns the compute timeline value of the copy.
    u64 PresentCompute(Frame* frame, vk::Image source, bool final, bool is_game_frame,
                       std::chrono::steady_clock::time_point present_at = {});
    void WaitCompute(u64 tick);
    /// bbport: BB_FRAME_STATS timestamps around frame generation commands.
    void FgTimestamp(vk::CommandBuffer cmdbuf, bool end);
    void GuardFgVram();

    void RecreateFrame(Frame* frame, u32 width, u32 height);

    void SetExpectedGameSize(s32 width, s32 height);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    std::deque<u64> recent_frame_ticks; ///< bbport: BB_FRAMES_AHEAD bound (PrepareFrame)
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
    std::unique_ptr<FrameGeneration> frame_generation;
    std::chrono::steady_clock::time_point last_flip_time{};
    /// bbport: when the real frame of the last frame generation pair was presented
    std::chrono::steady_clock::time_point last_real_present{};
    double real_interval_ms{1000.0 / 60.0}; ///< bbport: average interval of guest flips
    /// bbport: presentation of frame generation pairs on the compute queue
    /// (Instance::GetComputeQueue): its own command buffers, reused in turn, and a timeline
    /// semaphore counting its submissions. Null queue: no compute presentation.
    struct ComputePresent {
        vk::Queue queue;
        vk::UniqueCommandPool pool;
        vk::UniqueSemaphore timeline;
        std::array<vk::CommandBuffer, 8> buffers{};
        std::array<u64, 8> ticks{};
        u64 tick{};
        u32 next{};
    } compute_present;
    vk::UniqueQueryPool fg_queries; ///< bbport: BB_FRAME_STATS frame generation GPU time
    std::array<bool, 8> fg_query_used{}, fg_query_split{};
    u32 fg_query_slot{};
    float fg_query_period{1.0f};
    /// bbport: GuardFgVram: the last check, checks in a row near/clear of the budget, and the
    /// device usage when frame generation paused.
    std::chrono::steady_clock::time_point fg_vram_checked{};
    u32 fg_vram_full_checks{}, fg_vram_clear_checks{};
    u64 fg_vram_paused_usage{};
};

} // namespace Vulkan
