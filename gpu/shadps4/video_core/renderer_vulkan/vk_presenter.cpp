#include "bbport_nvidia.h"
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "bbport_background.h"
#include "bbframe_pacing.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <thread>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <vk_mem_alloc.h>
#ifdef MemoryBarrier
#undef MemoryBarrier // bbport: winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

namespace Vulkan {

namespace {
// bbport: BB_FRAME_STATS=1 also prints what reaches the swapchain every 5 s: real and generated
// presents per second, the spread of the intervals between them (what the display shows, which
// the guest flip stats cannot see with frame generation), where the swap thread waited, and the
// GPU time of frame generation.
class PresentStats {
public:
    using Clock = std::chrono::steady_clock;
    enum Wait { WaitDrawn, WaitGpu, WaitSleep, WaitAcquire, WaitPresent, WaitCount };

    bool Enabled() const {
        return enabled;
    }
    void Presented(bool generated) {
        if (!enabled) return;
        const auto now = Clock::now();
        if (last != Clock::time_point{}) {
            intervals.push_back(std::chrono::duration<double, std::milli>(now - last).count());
        }
        last = now;
        ++(generated ? generated_count : real_count);
        const double window = std::chrono::duration<double>(now - window_start).count();
        if (window >= 5.0) Print(window, now);
    }
    void Waited(Wait what, Clock::time_point since) {
        if (enabled) waits[what] += std::chrono::duration<double, std::milli>(Clock::now() - since).count();
    }
    void GpuMs(double ms, double prepare_ms) {
        gpu_ms += ms;
        ++gpu_samples;
        if (prepare_ms >= 0.0) {
            prepare_ms_total += prepare_ms;
            ++prepare_samples;
        }
    }

private:
    void Print(double window, Clock::time_point now) {
        std::vector<double> sorted = intervals;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0, sq = 0;
        for (const double ms : intervals) {
            sum += ms;
            sq += ms * ms;
        }
        const double mean = sorted.empty() ? 0.0 : sum / sorted.size();
        const auto pct = [&](u32 p) {
            return sorted.empty() ? 0.0 : sorted[std::min<size_t>(sorted.size() - 1, sorted.size() * p / 100)];
        };
        const u32 real = std::max(real_count, 1u);
        std::printf("Present stats: %.1f real + %.1f generated per s; intervals median %.2f ms, "
                    "stddev %.2f, p5 %.2f, p95 %.2f, max %.2f; swap thread ms per real frame: "
                    "frame drawn %.2f, generated copy %.2f, pacing sleep %.2f, "
                    "acquire %.2f, present %.2f; FG GPU %.2f ms (%.2f before the interpolation); "
                    "%.1f graphics queue submissions per real frame\n",
                    real_count / window, generated_count / window, pct(50),
                    std::sqrt(std::max(0.0, (sorted.empty() ? 0.0 : sq / sorted.size()) - mean * mean)),
                    pct(5), pct(95), sorted.empty() ? 0.0 : sorted.back(), waits[WaitDrawn] / real,
                    waits[WaitGpu] / real, waits[WaitSleep] / real,
                    waits[WaitAcquire] / real, waits[WaitPresent] / real,
                    gpu_samples ? gpu_ms / gpu_samples : 0.0,
                    prepare_samples ? prepare_ms_total / prepare_samples : 0.0,
                    double(Scheduler::graphics_submissions.exchange(0)) / real);
        intervals.clear();
        real_count = generated_count = 0;
        waits = {};
        gpu_ms = prepare_ms_total = 0;
        gpu_samples = prepare_samples = 0;
        window_start = now;
    }

    const bool enabled = EmulatorSettingsImpl::Flag("BB_FRAME_STATS", false);
    Clock::time_point window_start = Clock::now(), last{};
    std::vector<double> intervals;
    u32 real_count = 0, generated_count = 0;
    std::array<double, WaitCount> waits{};
    double gpu_ms = 0, prepare_ms_total = 0;
    u32 gpu_samples = 0, prepare_samples = 0;
};
PresentStats present_stats;

// bbport: the overlay's FPS with FSR frame generation: frames presented per second and the real
// ones among them, averaged over the last half second (BbSettings::fsr_fg_output_fps).
void CountFsrFgPresent(bool generated) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point window_start = Clock::now();
    static u32 presented = 0, real = 0;
    ++presented;
    real += generated ? 0 : 1;
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - window_start).count();
    if (seconds < 0.5) return;
    auto& s = BbSettings::Get();
    s.fsr_fg_output_fps = float(presented / seconds);
    s.fsr_fg_real_fps = float(real / seconds);
    presented = real = 0;
    window_start = now;
}

} // namespace

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// bbport: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    // bbport: presents on the present queue, which is the graphics family's second queue when
    // it has one (Instance::CreateDevice).
    present_scheduler.UseQueue(instance.GetPresentQueue());
    // bbport: frame generation is recorded in the draw stream right after its frame
    // (Generate): its resources retire with the draw queue's submissions.
    frame_generation=std::make_unique<FrameGeneration>(
        instance.GetPhysicalDevice(),instance.GetDevice(),instance.GetAllocator(),
        FrameGeneration::Submissions{
            [this] { draw_scheduler.Wait(draw_scheduler.CurrentTick() - 1); },
            [this](u64 tick) { draw_scheduler.Wait(tick); },
            [this](u64 tick) { return draw_scheduler.IsFree(tick); }});
    // bbport: the generated and real frames of a pair are presented by a copy on the compute
    // queue when it can present to the window (AMD: async compute at high priority). Their GPU
    // work then starts at once: on a graphics queue it waited behind the next frame's draws,
    // so the pair reached the screen late and back to back.
    if (const vk::Queue compute = instance.GetComputeQueue(); compute && swapchain.ComputePresent()) {
        const vk::Device device = instance.GetDevice();
        compute_present.queue = compute;
        compute_present.pool = Check(device.createCommandPoolUnique({
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = instance.GetComputeQueueFamilyIndex(),
        }));
        const vk::SemaphoreTypeCreateInfo timeline{
            .semaphoreType = vk::SemaphoreType::eTimeline, .initialValue = 0};
        compute_present.timeline = Check(device.createSemaphoreUnique({.pNext = &timeline}));
        const auto buffers = Check(device.allocateCommandBuffers({
            .commandPool = *compute_present.pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = u32(compute_present.buffers.size()),
        }));
        std::copy(buffers.begin(), buffers.end(), compute_present.buffers.begin());
        std::printf("Frame generation: pairs presented from the compute queue\n");
        // The pair is copied as it is: frame interpolation can work on the 8-bit sRGB frame.
        frame_generation->UseCompactColor(true);
    }
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);
    BbOverlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);

}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    // Streamline can still own the depth/motion images after our queue completes.
    BbNvidia::BeforeSwapchainChange();
    frame_generation->Reset();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    if (compute_present.queue) {
        WaitCompute(compute_present.tick);
    }
    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        if (frame.generated_image) {
            device.destroyImageView(frame.generated_view);
            vmaDestroyImage(instance.GetAllocator(), frame.generated_image, frame.generated_allocation);
        }
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }
    if (frame->generated_image) {
        device.destroyImageView(frame->generated_view);
        vmaDestroyImage(instance.GetAllocator(), frame->generated_image, frame->generated_allocation);
        frame->generated_image = vk::Image{};
        frame->generated_view = vk::ImageView{};
    }

    // bbport: the compute queue copies frames into the swapchain (PresentCompute).
    const std::array families{instance.GetGraphicsQueueFamilyIndex(),
                              instance.GetComputeQueueFamilyIndex()};
    const bool shared = compute_present.queue != vk::Queue{};
    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .queueFamilyIndexCount = shared ? u32(families.size()) : 0u,
        .pQueueFamilyIndices = shared ? families.data() : nullptr,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// bbport: raw copy of an image for BB_FRAME_DUMP_TRIGGER, written once the GPU is done.
static void DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                    vk::Image image, vk::ImageLayout layout, u32 w, u32 h, const char* name,
                    int index) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {w, h, 1}});
    const char* dir = std::getenv("BB_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", name,
                  index, w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    // bbport: BB_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and this frame's
    // guest display buffer and presented image are written to BB_DUMP_DIR (default out/dump)
    // as raw 32-bit pixels (menus and movies included; the upscaler dumps scene frames only).
    static const char* frame_dump_trigger = std::getenv("BB_FRAME_DUMP_TRIGGER");
    static int frame_dump_index = 0;
    const bool frame_dump = frame_dump_trigger && std::remove(frame_dump_trigger) == 0;
    // bbport: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(
        cpu_address, display, GetFrameViewFormat(attribute.attrib.pixel_format));
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        image_view = display.view;
        image_size = vk::Extent2D{display.width, display.height};
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    } else {
        auto& image = texture_cache.GetImage(image_id);
        if (frame_dump) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                    vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                    image.info.size.height, "display", frame_dump_index);
        }
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);
    if (frame_generation->Wanted()) {
        rasterizer->GetUpscaler().PrepareFrameGenerationInputs();
    }
    frame_generation->Capture(cmdbuf,rasterizer->GetUpscaler().FrameGenerationInputs(),
                              frame->image_view,frame->width,frame->height,
                              swapchain.GetSurfaceFormat().format,frame->frame_generation);
    Generate(frame, cmdbuf);
    if (frame_dump) {
        // The presented image as the swapchain blit reads it (General after the pass).
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "output", frame_dump_index);
        ++frame_dump_index;
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);

    // bbport: the GPU command thread runs at most BB_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    static const u32 frames_ahead = [] {
        const char* env = std::getenv("BB_FRAMES_AHEAD");
        return env ? u32(std::max(0, std::atoi(env))) : 1u;
    }();
    if (frames_ahead) {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    frame->generated = frame->overlay_drawn = frame->compute_present = false;

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    if (is_game_frame && !is_reusing_frame) {
        // bbport: the guest's real frame interval, from when its flips were released (the
        // frame limiter or the vblank), for the frame generation pacing below. Gaps over 100 ms
        // (loading, pauses) are not frames of the steady rate.
        if (frame->flip_time != std::chrono::steady_clock::time_point{}) {
            const double ms = std::chrono::duration<double, std::milli>(
                frame->flip_time - last_flip_time).count();
            if (last_flip_time != std::chrono::steady_clock::time_point{} && ms > 0.0 &&
                ms < 100.0) {
                real_interval_ms += (ms - real_interval_ms) * 0.1;
            }
            last_flip_time = frame->flip_time;
        }
    }
    BbBackground::Wait();
    if (is_game_frame && frame) {
        BbSettings::Get().active_output_width = frame->width;
        BbSettings::Get().active_output_height = frame->height;
    }
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };
    const auto discard_unpresented = [&] {
        frame_generation->Invalidate();
        if (!is_reusing_frame) {
            // No presentation will signal a new fence for this frame. Its draw
            // submission must finish before the slot, including FG input copies,
            // can be reused or destroyed after minimization/acquisition failure.
            const vk::SemaphoreWaitInfo ready{.semaphoreCount=1,
                .pSemaphores=&frame->ready_semaphore,.pValues=&frame->ready_tick};
            Check(instance.GetDevice().waitSemaphores(ready,std::numeric_limits<u64>::max()));
        }
        free_frame();
    };

    // bbport: a minimized window has no pixels on Windows (0x0); keep the swapchain and skip
    // presenting until the window is restored.
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        discard_unpresented();
        return;
    }

    // Recreate the swapchain if the window was resized.
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight() ||
        swapchain.WindowPolicyChanged()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    // bbport: no swapchain while the surface is 0x0 (Swapchain::Create; SDL can still report the
    // window's size while it is minimized): skip quietly, as for a minimized window.
    if (swapchain.GetWidth() == 0) {
        discard_unpresented();
        return;
    }

    auto acquire_since = PresentStats::Clock::now();
    const bool acquired = swapchain.AcquireNextImage();
    present_stats.Waited(PresentStats::WaitAcquire, acquire_since);
    if (!acquired) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (swapchain.GetWidth() == 0) {
            discard_unpresented();
            return;
        }
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            discard_unpresented();
            return;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    auto submit_image = [&](vk::Image source_image, bool final) {
        // bbport: the game frame is blitted (letterboxed) straight into the swapchain image.
        const vk::Image swapchain_image = swapchain.Image();
        auto& scheduler = present_scheduler;
        const auto cmdbuf = scheduler.CommandBuffer();

        if (EmulatorSettings.IsVkHostMarkersEnabled()) {
            cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
                .pLabelName = "Present",
            });
        }

        {
            const vk::Extent2D extent = swapchain.GetExtent();
            SetExpectedGameSize(s32(extent.width), s32(extent.height));
            const vk::ImageSubresourceRange color_range{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            };
            const std::array pre_barriers{
                vk::ImageMemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eNone,
                    .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .oldLayout = vk::ImageLayout::eUndefined,
                    .newLayout = vk::ImageLayout::eTransferDstOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = swapchain_image,
                    .subresourceRange = color_range,
                },
                vk::ImageMemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                    .oldLayout = vk::ImageLayout::eGeneral,
                    .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = source_image,
                    .subresourceRange = color_range,
                },
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                   vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                                   {}, {}, pre_barriers);
            // bbport: black bars only where the frame leaves them; the frame normally follows the
            // swapchain's size and covers it (a full-screen clear is 8 MiB of writes a frame at
            // 1080p, which an integrated GPU's shared memory pays for).
            const vk::Rect2D fit = FitImage(frame->width, frame->height, extent.width, extent.height);
            if (fit.offset.x != 0 || fit.offset.y != 0 || fit.extent != extent) {
                const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
                cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black,
                                       color_range);
                const vk::MemoryBarrier clear_done{
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                };
                cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                       vk::PipelineStageFlagBits::eTransfer,
                                       vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
            }
            cmdbuf.blitImage(source_image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                             vk::ImageLayout::eTransferDstOptimal,
                             MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height),
                             vk::Filter::eLinear);
            // bbport: the settings menu / FPS counter over the frame, at display resolution
            // (frame generation pairs have it already: Generate).
            const bool overlay = BbOverlay::Visible() && !(is_game_frame && frame->overlay_drawn);
            const std::array post_barriers{
                vk::ImageMemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                                   vk::AccessFlagBits::eColorAttachmentWrite
                                             : vk::AccessFlagBits::eNone,
                    .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                    .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                         : vk::ImageLayout::ePresentSrcKHR,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = swapchain_image,
                    .subresourceRange = color_range,
                },
                vk::ImageMemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                    .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                    .newLayout = vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = source_image,
                    .subresourceRange = color_range,
                },
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                   vk::PipelineStageFlagBits::eAllCommands,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
            if (overlay) {
                BbOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
                const vk::ImageMemoryBarrier to_present{
                    .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                    .dstAccessMask = vk::AccessFlagBits::eNone,
                    .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                    .newLayout = vk::ImageLayout::ePresentSrcKHR,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = swapchain_image,
                    .subresourceRange = color_range,
                };
                cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                       vk::PipelineStageFlagBits::eBottomOfPipe,
                                       vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
            }
        }
        if (EmulatorSettings.IsVkHostMarkersEnabled()) {
            cmdbuf.endDebugUtilsLabelEXT();
        }

        BbNvidia::BeforePresent(frame,cmdbuf,swapchain.Image(),swapchain.ImageView(),
                                 swapchain.GetSurfaceFormat().format,swapchain.GetImageCount(),
                                 is_game_frame && !is_reusing_frame);
        // Flush vulkan commands.

        SubmitInfo info{};
        if (const vk::Semaphore acquired = swapchain.GetImageAcquiredSemaphore()) {
            info.AddWait(acquired);
        } else {
            swapchain.WaitAcquired();
        }
        info.AddWait(frame->ready_semaphore, frame->ready_tick);
        info.AddSignal(swapchain.GetPresentReadySemaphore());
        if (final) info.AddSignal(frame->present_done);
        scheduler.Flush(info);

        // Present to swapchain.
        {
            std::scoped_lock submit_lock{Scheduler::submit_mutex};
            const bool presented=BbNvidia::Present(frame,[&] {
                BbAmd::BeforePresent(frame->amd_token, final && is_game_frame && !is_reusing_frame);
                const auto since = PresentStats::Clock::now();
                const bool ok = swapchain.Present();
                present_stats.Waited(PresentStats::WaitPresent, since);
                return ok;
            });
            if (!presented && window.GetWidth() != 0 && window.GetHeight() != 0) {
                swapchain.Recreate(window.GetWidth(), window.GetHeight());
            }
        }
    };
    if (!is_game_frame || is_reusing_frame) frame_generation->Invalidate();
    const bool record_fg = is_game_frame && !is_reusing_frame;
    // bbport: frame generation ran in the draw stream (Generate). A pair whose sizes still
    // match the swapchain is presented from the compute queue; otherwise by the blits here.
    const bool generated = record_fg && frame->generated;
    std::chrono::steady_clock::time_point real_due{}; ///< the real frame's present time (pairs)
    const bool compute = record_fg && frame->compute_present &&
        frame->width == swapchain.GetExtent().width && frame->height == swapchain.GetExtent().height;
    if (generated) {
        // bbport: the generated frame goes out once its frame is drawn (frame generation ran in
        // the same submission), the real one half a real-frame interval later. The wait is for
        // the GPU's own work only, so slow GPU work cannot bunch the two frames into one
        // presentation interval. Not for the presentation copy: it waits for the swapchain
        // image, which FIFO releases only when the display moves past it (17 ms in fullscreen
        // with three images), and pacing from it put that latency into every frame.
        // The half interval comes from the guest's flips: measured between presents it included
        // this very sleep and grew until its clamp (17 real FPS).
        // No two presents back to back either, also when the next frame is drawn early: in flip
        // mode (fullscreen, borderless) a present behind one not flipped yet waits for the
        // vblank, the swap thread fell behind for good and the pairs went out together (~94
        // presents/s, 47 real FPS, where a window did 55). The generated frame keeps 0.8 of the
        // half interval after the previous real one: the margin lets a swap thread that holds
        // the game back catch up, so the flip interval shrinks back to what the GPU does.
        auto since = PresentStats::Clock::now();
        const vk::SemaphoreWaitInfo drawn{.semaphoreCount = 1,
            .pSemaphores = &frame->ready_semaphore, .pValues = &frame->ready_tick};
        Check(instance.GetDevice().waitSemaphores(drawn, std::numeric_limits<u64>::max()));
        present_stats.Waited(PresentStats::WaitDrawn, since);
        const auto half_period = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double, std::milli>(std::clamp(real_interval_ms * 0.5, 1.0, 25.0)));
        // Each frame goes out at its time: the wait for its swapchain image comes first. Waited
        // inside the present, it pushed the generated frame late while the real one kept its
        // time, and DWM shows one image per refresh: two presents in one refresh lost the first
        // (a screen probe saw 80 distinct images/s of 108 presented).
        acquire_since = PresentStats::Clock::now();
        swapchain.WaitAcquired();
        present_stats.Waited(PresentStats::WaitAcquire, acquire_since);
        // Not two presents within one display refresh either: the compositor shows one image
        // per refresh, and of two presents in one the first was never seen (a screen probe
        // counted 88 shown of 108 presented with 0.8 of the half interval at 144 Hz).
        static const auto refresh = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(1.05 / [] {
                const long hz = EmulatorSettingsImpl::Number("BB_FULLSCREEN_HZ", 0);
                return double(hz > 0 ? u32(hz) : BbDisplayRefreshHz());
            }()));
        const auto spacing = std::max(half_period * 4 / 5, refresh);
        const auto generated_due = last_real_present != std::chrono::steady_clock::time_point{}
            ? last_real_present + spacing : std::chrono::steady_clock::now();
        since = PresentStats::Clock::now();
        if (compute) {
            PresentCompute(frame, frame->generated_image, false, is_game_frame, generated_due);
        } else {
            BbFramePacing::SleepUntil(generated_due);
            submit_image(frame->overlay_drawn ? frame->generated_image
                                              : frame->frame_generation->PresentImage(), false);
        }
        present_stats.Presented(true);
        CountFsrFgPresent(true);
        present_stats.Waited(PresentStats::WaitGpu, since);
        const auto generated_at = std::max(generated_due, since);
        // bbport: the real frame's image now, while the pacing sleep runs: in flip mode
        // (fullscreen, or a window Windows promotes) FIFO holds images for a vblank or two, and
        // acquiring after the sleep pushed the real frame late (47 real FPS, 8 ms per frame here).
        acquire_since = PresentStats::Clock::now();
        swapchain.AcquireNextImage(true);
        swapchain.WaitAcquired();
        present_stats.Waited(PresentStats::WaitAcquire, acquire_since);
        real_due = generated_at + std::max(half_period, refresh);
        if (!compute) {
            since = PresentStats::Clock::now();
            BbFramePacing::SleepUntil(real_due);
            present_stats.Waited(PresentStats::WaitSleep, since);
        }
        BbBackground::Wait();
        const auto finish_without_real = [&] {
            // The generated frame has been presented. Signal the frame's final fence
            // even if the window loses its surface before the real presentation.
            SubmitInfo done{}; done.AddSignal(frame->present_done);
            present_scheduler.Flush(done);
            frame_generation->Invalidate(); free_frame();
        };
        if (window.GetWidth()==0 || window.GetHeight()==0) {
            finish_without_real(); return;
        }
        acquire_since = PresentStats::Clock::now();
        const bool acquired_real = swapchain.AcquireNextImage();
        present_stats.Waited(PresentStats::WaitAcquire, acquire_since);
        if (!acquired_real) {
            swapchain.Recreate(window.GetWidth(),window.GetHeight());
            if (!swapchain.AcquireNextImage()) {
                // Finish the real frame's ownership even when the second WSI
                // acquisition fails. The first submission already contains FI/OF.
                finish_without_real(); return;
            }
        }
    }
    // The pacing anchor is when the real frame goes out (its due time), not after its copy and
    // present: with their time included the cycle came to more than the measured interval and
    // drifted down.
    last_real_present = generated ? std::max(real_due, std::chrono::steady_clock::now())
                                  : std::chrono::steady_clock::time_point{};
    // A swapchain recreated between the two frames of a pair can have another size.
    if (compute && frame->width == swapchain.GetExtent().width &&
        frame->height == swapchain.GetExtent().height) {
        PresentCompute(frame, frame->image, true, is_game_frame, real_due);
    } else {
        submit_image(frame->image,true);
    }
    if (is_game_frame && !is_reusing_frame) {
        present_stats.Presented(false);
        CountFsrFgPresent(false);
    }
    free_frame();
    // bbport: and the next pair's first image while the next frame is drawn.
    if (generated && window.GetWidth() != 0 && window.GetHeight() != 0) {
        acquire_since = PresentStats::Clock::now();
        swapchain.AcquireNextImage(true);
        present_stats.Waited(PresentStats::WaitAcquire, acquire_since);
    }
}

void Presenter::FgTimestamp(vk::CommandBuffer cmdbuf, bool end) {
    // bbport: BB_FRAME_STATS: GPU time of frame generation from timestamps around its commands,
    // read back once the slot comes round again (8 generated frames later, long since done).
    if (!present_stats.Enabled()) return;
    if (!fg_queries) {
        fg_queries = Check(instance.GetDevice().createQueryPoolUnique(
            {.queryType = vk::QueryType::eTimestamp, .queryCount = 24}));
        fg_query_period = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
        // Also between the optical flow (prepare) and the interpolation.
        frame_generation->between_passes = [this](vk::CommandBuffer cmd) {
            cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *fg_queries,
                                16 + fg_query_slot);
            fg_query_split[fg_query_slot] = true;
        };
    }
    if (!end) {
        fg_query_slot = (fg_query_slot + 1) % 8;
        if (fg_query_used[fg_query_slot]) {
            std::array<u64, 2> ticks{};
            u64 split{};
            const auto device = instance.GetDevice();
            if (device.getQueryPoolResults(*fg_queries, fg_query_slot * 2, 2, sizeof(ticks),
                                           ticks.data(), sizeof(u64),
                                           vk::QueryResultFlagBits::e64) == vk::Result::eSuccess) {
                const bool has_split =
                    fg_query_split[fg_query_slot] &&
                    device.getQueryPoolResults(*fg_queries, 16 + fg_query_slot, 1, sizeof(split),
                                               &split, sizeof(u64),
                                               vk::QueryResultFlagBits::e64) == vk::Result::eSuccess;
                present_stats.GpuMs((ticks[1] - ticks[0]) * fg_query_period / 1e6,
                                    has_split ? (split - ticks[0]) * fg_query_period / 1e6 : -1.0);
            }
        }
        cmdbuf.resetQueryPool(*fg_queries, fg_query_slot * 2, 2);
        cmdbuf.resetQueryPool(*fg_queries, 16 + fg_query_slot, 1);
        fg_query_split[fg_query_slot] = false;
    }
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *fg_queries,
                           fg_query_slot * 2 + (end ? 1 : 0));
    fg_query_used[fg_query_slot] = end;
}

void Presenter::WaitCompute(u64 tick) {
    const vk::Semaphore timeline = *compute_present.timeline;
    const vk::SemaphoreWaitInfo wait{.semaphoreCount = 1, .pSemaphores = &timeline, .pValues = &tick};
    Check(instance.GetDevice().waitSemaphores(wait, std::numeric_limits<u64>::max()));
}

void Presenter::Generate(Frame* frame, vk::CommandBuffer cmdbuf) {
    // bbport: frame interpolation of this frame right after its own draws, in the draw stream:
    // the generated frame is ready with the real one, and no later than the next frame's draws
    // (recorded on the presentation queue it waited behind them: 17 real FPS at most).
    // Record also tears the context down once frame generation is turned off.
    frame->generated = false;
    frame->overlay_drawn = false;
    frame->compute_present = false;
    if (frame->frame_generation) FgTimestamp(cmdbuf, false);
    frame->generated = frame_generation->Record(cmdbuf, frame->frame_generation);
    if (frame->frame_generation) FgTimestamp(cmdbuf, true);
    frame_generation->Submitted(draw_scheduler.CurrentTick());
    if (!frame->frame_generation && frame->generated_image) {
        // Frame generation is off: its copy of this frame goes (the frame is reused only after
        // its present_done, so nothing uses it).
        instance.GetDevice().destroyImageView(frame->generated_view);
        vmaDestroyImage(instance.GetAllocator(), frame->generated_image, frame->generated_allocation);
        frame->generated_image = vk::Image{};
        frame->generated_view = vk::ImageView{};
    }
    if (!frame->frame_generation || !compute_present.queue) {
        return;
    }
    // A pair presented from the compute queue is copied as it is: the generated frame goes
    // into the swapchain format here, and the overlay is drawn into both now (it needs a
    // graphics queue; drawn after Capture, frame generation never sees it).
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const auto barrier = [&](vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                             vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage, .srcAccessMask = src_access,
            .dstStageMask = dst_stage, .dstAccessMask = dst_access,
            .oldLayout = from, .newLayout = to, .image = image, .subresourceRange = range};
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto memory = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    if (frame->generated) {
        // 8-bit sRGB frame generation writes encoded values: the blit into the copy source
        // must not encode them again, so that image is the swapchain format's UNORM twin.
        const vk::Format format = swapchain.GetSurfaceFormat().format;
        const vk::Format image_format = !frame->frame_generation->srgb8 ? format
            : format == vk::Format::eB8G8R8A8Srgb ? vk::Format::eB8G8R8A8Unorm
            : format == vk::Format::eR8G8B8A8Srgb ? vk::Format::eR8G8B8A8Unorm : format;
        if (frame->generated_image && frame->generated_format != image_format) {
            // The frame is reused only after its present_done: nothing uses the image now.
            instance.GetDevice().destroyImageView(frame->generated_view);
            vmaDestroyImage(instance.GetAllocator(), frame->generated_image,
                            frame->generated_allocation);
            frame->generated_image = vk::Image{};
        }
        if (!frame->generated_image) {
            const std::array families{instance.GetGraphicsQueueFamilyIndex(),
                                      instance.GetComputeQueueFamilyIndex()};
            const VkImageCreateInfo info = vk::ImageCreateInfo{
                .flags = vk::ImageCreateFlagBits::eMutableFormat,
                .imageType = vk::ImageType::e2D,
                .format = image_format,
                .extent = {frame->width, frame->height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .usage = vk::ImageUsageFlagBits::eColorAttachment |
                         vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc,
                .sharingMode = vk::SharingMode::eConcurrent,
                .queueFamilyIndexCount = u32(families.size()),
                .pQueueFamilyIndices = families.data(),
            };
            const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
            VkImage image{};
            if (vmaCreateImage(instance.GetAllocator(), &info, &alloc, &image,
                               &frame->generated_allocation, nullptr) != VK_SUCCESS) {
                return; // presented by the blits instead
            }
            frame->generated_image = vk::Image{image};
            frame->generated_format = image_format;
            frame->generated_view = Check(instance.GetDevice().createImageView({
                .image = frame->generated_image, .viewType = vk::ImageViewType::e2D,
                .format = format, .subresourceRange = range}));
        }
        const vk::Image source = frame->frame_generation->PresentImage();
        barrier(source, vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral, all,
                vk::AccessFlagBits2::eMemoryWrite, vk::PipelineStageFlagBits2::eBlit,
                vk::AccessFlagBits2::eTransferRead);
        barrier(frame->generated_image, vk::ImageLayout::eUndefined,
                vk::ImageLayout::eTransferDstOptimal, all, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite);
        const s32 w = s32(frame->width), h = s32(frame->height);
        const vk::ImageBlit region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{w, h, 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{w, h, 1}},
        };
        cmdbuf.blitImage(source, vk::ImageLayout::eGeneral, frame->generated_image,
                         vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
        barrier(frame->generated_image, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eBlit,
                vk::AccessFlagBits2::eTransferWrite, all, memory);
    }
    if (BbOverlay::Visible()) {
        const vk::Extent2D extent{frame->width, frame->height};
        const auto color = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        const auto color_access = vk::AccessFlagBits2::eColorAttachmentRead |
                                  vk::AccessFlagBits2::eColorAttachmentWrite;
        barrier(frame->image, vk::ImageLayout::eGeneral, vk::ImageLayout::eColorAttachmentOptimal,
                all, memory, color, color_access);
        if (frame->generated) {
            barrier(frame->generated_image, vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eColorAttachmentOptimal, all, memory, color, color_access);
        }
        BbOverlay::Render(cmdbuf, frame->image_view, extent,
                          frame->generated ? frame->generated_view : vk::ImageView{});
        barrier(frame->image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eGeneral,
                color, color_access, all, memory);
        if (frame->generated) {
            barrier(frame->generated_image, vk::ImageLayout::eColorAttachmentOptimal,
                    vk::ImageLayout::eGeneral, color, color_access, all, memory);
        }
    }
    frame->overlay_drawn = true;
    frame->compute_present = true;
    // bbport: BB_FG_DUMP_TRIGGER=<file>: when it exists it is removed and the next
    // BB_FG_DUMP_COUNT (default 4) pairs are written to BB_DUMP_DIR as raw 32-bit pixels.
    static const char* fg_dump = std::getenv("BB_FG_DUMP_TRIGGER");
    static int fg_dump_remaining = 0, fg_dump_index = 0;
    if (fg_dump && fg_dump_remaining == 0 && std::remove(fg_dump) == 0) {
        const char* count = std::getenv("BB_FG_DUMP_COUNT");
        fg_dump_remaining = count ? std::max(1, std::atoi(count)) : 4;
    }
    if (fg_dump_remaining > 0 && frame->generated) {
        --fg_dump_remaining;
        const vk::MemoryBarrier2 done{.srcStageMask = all, .srcAccessMask = memory,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->generated_image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "fg_generated", fg_dump_index);
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "fg_real", fg_dump_index);
        ++fg_dump_index;
    }
}

u64 Presenter::PresentCompute(Frame* frame, vk::Image source, bool final, bool is_game_frame,
                              std::chrono::steady_clock::time_point present_at) {
    // bbport: a copy into the acquired swapchain image on the compute queue, and its present
    // (same size and format: frame images follow the swapchain; Present checks the size).
    auto& cp = compute_present;
    const u32 slot = cp.next;
    cp.next = (cp.next + 1) % u32(cp.buffers.size());
    WaitCompute(cp.ticks[slot]);
    const vk::CommandBuffer cmdbuf = cp.buffers[slot];
    Check(cmdbuf.reset());
    Check(cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
    const vk::Image target = swapchain.Image();
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::ImageMemoryBarrier2 to_copy{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = target,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy});
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {frame->width, frame->height, 1},
    };
    cmdbuf.copyImage(source, vk::ImageLayout::eGeneral, target,
                     vk::ImageLayout::eTransferDstOptimal, region);
    const vk::ImageMemoryBarrier2 to_present{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eNone,
        .dstAccessMask = vk::AccessFlagBits2::eNone,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::ePresentSrcKHR,
        .image = target,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_present});
    Check(cmdbuf.end());
    const u64 tick = cp.tick + 1;
    std::vector<vk::SemaphoreSubmitInfo> waits{
        vk::SemaphoreSubmitInfo{.semaphore = frame->ready_semaphore, .value = frame->ready_tick,
                                .stageMask = vk::PipelineStageFlagBits2::eCopy},
    };
    if (const vk::Semaphore acquired = swapchain.GetImageAcquiredSemaphore()) {
        waits.push_back({.semaphore = acquired, .stageMask = vk::PipelineStageFlagBits2::eCopy});
    } else {
        const auto since = PresentStats::Clock::now();
        swapchain.WaitAcquired();
        present_stats.Waited(PresentStats::WaitAcquire, since);
    }
    const std::array signals{
        vk::SemaphoreSubmitInfo{.semaphore = swapchain.GetPresentReadySemaphore(),
                                .stageMask = vk::PipelineStageFlagBits2::eAllCommands},
        vk::SemaphoreSubmitInfo{.semaphore = *cp.timeline, .value = tick,
                                .stageMask = vk::PipelineStageFlagBits2::eAllCommands},
    };
    const vk::CommandBufferSubmitInfo buffer{.commandBuffer = cmdbuf};
    {
        std::scoped_lock lock{Scheduler::submit_mutex};
        Check(cp.queue.submit2(vk::SubmitInfo2{
            .waitSemaphoreInfoCount = u32(waits.size()), .pWaitSemaphoreInfos = waits.data(),
            .commandBufferInfoCount = 1, .pCommandBufferInfos = &buffer,
            .signalSemaphoreInfoCount = u32(signals.size()), .pSignalSemaphoreInfos = signals.data(),
        }, final ? frame->present_done : vk::Fence{}));
    }
    // The copy runs ahead; the present waits for its time. A copy submitted at the present time
    // finished a little sooner or later, and two frames ready within one refresh lost the first.
    if (present_at != std::chrono::steady_clock::time_point{}) {
        const auto since = PresentStats::Clock::now();
        BbFramePacing::SleepUntil(present_at);
        present_stats.Waited(PresentStats::WaitSleep, since);
    }
    {
        std::scoped_lock lock{Scheduler::submit_mutex};
        BbAmd::BeforePresent(frame->amd_token, final && is_game_frame);
        // The present goes to the present queue like every other present, so presents keep
        // the order they are made in when frames switch between the two ways.
        const auto since = PresentStats::Clock::now();
        const bool presented = swapchain.Present();
        present_stats.Waited(PresentStats::WaitPresent, since);
        if (!presented && window.GetWidth() != 0 && window.GetHeight() != 0) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }
    cp.tick = tick;
    cp.ticks[slot] = tick;
    return tick;
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    BbNvidia::WaitForInputs(frame);
    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // no surface (minimized window): keep the last frame size
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
