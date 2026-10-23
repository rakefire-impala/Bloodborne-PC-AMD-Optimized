#include "bbport_nvidia.h"
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <limits>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "bbport_settings.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "bbport_splash.h"
#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif

namespace Vulkan {

static constexpr vk::SurfaceFormatKHR SURFACE_FORMAT_HDR = {
    .format = vk::Format::eA2B10G10R10UnormPack32,
    .colorSpace = vk::ColorSpaceKHR::eHdr10St2084EXT,
};

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window_)
    : instance{instance_}, window{window_}, surface{CreateSurface(instance.GetInstance(), window)} {
    if (const u32 family = instance.GetComputeQueueFamilyIndex(); family != ~0u) {
        const auto [result, supported] =
            instance.GetPhysicalDevice().getSurfaceSupportKHR(family, surface);
        compute_present = result == vk::Result::eSuccess && supported;
    }
    FindPresentFormat();
    Create(window.GetWidth(), window.GetHeight());
    ImGui::Core::Initialize(instance, window, image_count, surface_format.format);
}

Swapchain::~Swapchain() {
    Destroy();
    instance.GetInstance().destroySurfaceKHR(surface);
}

bool Swapchain::SurfaceHasArea() const {
    const auto [result, capabilities] =
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    if (result != vk::Result::eSuccess) {
        return true; // Create reports the failure
    }
    const vk::Extent2D current = capabilities.currentExtent;
    return current.width == std::numeric_limits<u32>::max() ||
           (current.width != 0 && current.height != 0);
}

void Swapchain::Create(u32 width_, u32 height_) {
    // bbport: a minimized window's surface is 0x0 on Windows, and stays so for a moment after
    // SDL reports the window restored (a screenshot overlay minimizes a fullscreen game). The
    // swapchain was then created at 0x0 and drawn into, and AMD's integrated GPU driver lost the
    // device. Keep the current swapchain (or none) and try again on the next frame: the
    // presenter skips frames until the surface has a size.
    if (!SurfaceHasArea()) {
        width = height = 0;
        needs_recreation = true;
        if (!reported_empty_surface) {
            reported_empty_surface = true;
            std::printf("Swapchain: the window's surface is 0x0 (minimized); presenting resumes "
                        "when it has a size\n");
        }
        return;
    }
    width = width_;
    height = height_;
    needs_recreation = false;
    acquired = false;

    Destroy();
    window_mode = window.GetWindowMode();
    borderless_vsync = BbSettings::Get().borderless_vsync;
    FindPresentMode();
    SetSurfaceProperties();

    // bbport: the compute family copies frame generation pairs in (Presenter::PresentCompute).
    std::array<u32, 3> queue_family_indices = {
        instance.GetGraphicsQueueFamilyIndex(),
        instance.GetPresentQueueFamilyIndex(),
    };
    u32 queue_family_indices_count =
        queue_family_indices[0] == queue_family_indices[1] ? 1u : 2u;
    if (compute_present) {
        queue_family_indices[queue_family_indices_count++] = instance.GetComputeQueueFamilyIndex();
    }
    const bool exclusive = queue_family_indices_count == 1;
    const vk::SharingMode sharing_mode =
        exclusive ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent;
    const auto format = needs_hdr ? SURFACE_FORMAT_HDR : surface_format;
    vk::SwapchainCreateInfoKHR swapchain_info = {
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = sharing_mode,
        .queueFamilyIndexCount = queue_family_indices_count,
        .pQueueFamilyIndices = queue_family_indices.data(),
        .preTransform = transform,
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = true,
        .oldSwapchain = nullptr,
    };
#ifdef _WIN32
    // Prevent the Vulkan driver taking implicit exclusive ownership during borderless
    // presentation. Windows can still promote this opaque desktop-sized swapchain to
    // independent flip; its actual presentation path is a driver/OS decision.
    const VkSurfaceFullScreenExclusiveInfoEXT fullscreen_policy = {
        VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT, nullptr,
        VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT,
    };
    if (window_mode == BbSettings::Borderless && instance.HasFullScreenExclusivePolicy()) {
        swapchain_info.pNext = &fullscreen_policy;
    }
#endif

    auto [swapchain_result, chain] = instance.GetDevice().createSwapchainKHR(swapchain_info);
    // bbport: recreated at the VRAM budget (window mode change, Alt-Tab), the driver refused
    // the images and the game stopped; free staging, then the VRAM reserve, as for images.
    for (u32 attempt = 0; attempt < 4 &&
                          (swapchain_result == vk::Result::eErrorOutOfDeviceMemory ||
                           swapchain_result == vk::Result::eErrorOutOfHostMemory) &&
                          VideoCore::HandleOutOfMemory();
         ++attempt) {
        std::tie(swapchain_result, chain) = instance.GetDevice().createSwapchainKHR(swapchain_info);
    }
    ASSERT_MSG(swapchain_result == vk::Result::eSuccess, "Failed to create swapchain: {}",
               vk::to_string(swapchain_result));
    swapchain = chain;

    SetupImages();
    BbNvidia::BindDevice(instance.GetPhysicalDevice(),instance.GetDevice());
    RefreshSemaphores();
    LOG_INFO(Render_Vulkan, "Window mode {}, present mode {}, extent {}x{}", window_mode,
             vk::to_string(present_mode), extent.width, extent.height);
}

bool Swapchain::WindowPolicyChanged() const {
    return window_mode != window.GetWindowMode() ||
        (window_mode == BbSettings::Borderless &&
         borderless_vsync != BbSettings::Get().borderless_vsync);
}

void Swapchain::Recreate(u32 width_, u32 height_) {
    LOG_DEBUG(Render_Vulkan, "Recreate the swapchain: width={} height={} HDR={}", width_, height_,
              needs_hdr);
    Create(width_, height_);
}

void Swapchain::SetHDR(bool hdr) {
    if (needs_hdr == hdr) {
        return;
    }

    auto result = instance.GetDevice().waitIdle();
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(ImGui, "Failed to wait for Vulkan device idle on mode change: {}",
                    vk::to_string(result));
    }

    needs_hdr = hdr;
    Recreate(width, height);
    ImGui::Core::OnSurfaceFormatChange(needs_hdr ? SURFACE_FORMAT_HDR.format
                                                 : surface_format.format);
}

bool Swapchain::AcquireNextImage(bool fenced) {
    if (!swapchain || width == 0 || height == 0) {
        needs_recreation = true; // no swapchain of the surface's size yet (Create)
        return false;
    }
    if (acquired && !needs_recreation) {
        return true;
    }
    vk::Device device = instance.GetDevice();
    if (fenced && !acquire_fence) {
        acquire_fence = Check(device.createFence({}));
    }
    vk::Result result = device.acquireNextImageKHR(
        swapchain, std::numeric_limits<u64>::max(),
        fenced ? vk::Semaphore{} : image_acquired[frame_index],
        fenced ? acquire_fence : vk::Fence{}, &image_index);
    if (fenced && (result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR)) {
        fence_pending = true;
    }

    switch (result) {
    case vk::Result::eSuccess:
        acquired = true;
        acquired_fenced = fenced;
        break;
    case vk::Result::eSuboptimalKHR:
    case vk::Result::eErrorSurfaceLostKHR:
    case vk::Result::eErrorOutOfDateKHR:
    case vk::Result::eErrorUnknown:
        needs_recreation = true;
        break;
    default:
        LOG_CRITICAL(Render_Vulkan, "Swapchain acquire returned unknown result {}",
                     vk::to_string(result));
        UNREACHABLE();
        break;
    }

    return !needs_recreation;
}

bool Swapchain::Present(vk::Queue queue) {
    const vk::PresentInfoKHR present_info = {
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };

    acquired = false;
    acquired_fenced = false;
    auto result = (queue ? queue : instance.GetPresentQueue()).presentKHR(present_info);
    BbSplash::OnFramePresented(); // bbport: the startup splash ends at the first frame
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        needs_recreation = true;
    } else {
        ASSERT_MSG(result == vk::Result::eSuccess, "Swapchain presentation failed: {}",
                   vk::to_string(result));
    }

    frame_index = (frame_index + 1) % u32(image_acquired.size());

    return !needs_recreation;
}

void Swapchain::FindPresentFormat() {
    const auto [formats_result, formats] =
        instance.GetPhysicalDevice().getSurfaceFormatsKHR(surface);
    ASSERT_MSG(formats_result == vk::Result::eSuccess, "Failed to query surface formats: {}",
               vk::to_string(formats_result));

    // Check if the device supports HDR formats. Here we care of Rec.2020 PQ only as it is expected
    // game output. Other variants as e.g. linear Rec.2020 will require additional color space
    // rotation
    supports_hdr =
        std::find_if(formats.begin(), formats.end(), [](const vk::SurfaceFormatKHR& format) {
            return format == SURFACE_FORMAT_HDR;
        }) != formats.end();
    // Also make sure that user allowed us to use HDR
    supports_hdr &= EmulatorSettings.IsHdrAllowed();

    // If there is a single undefined surface format, the device doesn't care, so we'll just use
    // RGBA sRGB.
    if (formats[0].format == vk::Format::eUndefined) {
        surface_format.format = vk::Format::eR8G8B8A8Unorm;
        surface_format.colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
        return;
    }

    // Try to find a suitable format.
    for (const vk::SurfaceFormatKHR& sformat : formats) {
        vk::Format format = sformat.format;
        if (format != vk::Format::eR8G8B8A8Unorm && format != vk::Format::eB8G8R8A8Unorm) {
            continue;
        }

        surface_format.format = format;
        surface_format.colorSpace = sformat.colorSpace;
        return;
    }

    UNREACHABLE_MSG("Unable to find required swapchain format!");
}

void Swapchain::FindPresentMode() {
    auto [modes_result, modes] =
        instance.GetPhysicalDevice().getSurfacePresentModesKHR(surface);
#ifdef _WIN32
    if (window_mode == BbSettings::Borderless && instance.HasFullScreenExclusivePolicy()) {
        const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT>(
            instance.GetInstance().getProcAddr("vkGetPhysicalDeviceSurfacePresentModes2EXT"));
        const VkSurfaceFullScreenExclusiveInfoEXT policy = {
            VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT, nullptr,
            VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT,
        };
        const VkPhysicalDeviceSurfaceInfo2KHR info = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, &policy, surface,
        };
        uint32_t count = 0;
        if (query && query(instance.GetPhysicalDevice(), &info, &count, nullptr) == VK_SUCCESS) {
            std::vector<VkPresentModeKHR> available(count);
            const VkResult result = query(instance.GetPhysicalDevice(), &info, &count, available.data());
            modes_result = static_cast<vk::Result>(result);
            modes.clear();
            for (uint32_t i = 0; i < count && result == VK_SUCCESS; ++i) {
                modes.push_back(static_cast<vk::PresentModeKHR>(available[i]));
            }
        } else {
            modes_result = vk::Result::eErrorUnknown;
        }
    }
#endif
    if (modes_result != vk::Result::eSuccess) {
        LOG_ERROR(Render, "Failed to query available present modes, falling back to Fifo as "
                          "guaranteed supported option.");
        present_mode = vk::PresentModeKHR::eFifo;
        return;
    }

    const auto requested_mode = window_mode == BbSettings::Borderless
        ? std::string{borderless_vsync ? "Mailbox" : "Immediate"}
        : EmulatorSettings.GetPresentMode();
    if (requested_mode == "Mailbox") {
        present_mode = vk::PresentModeKHR::eMailbox;
    } else if (requested_mode == "Fifo") {
        present_mode = vk::PresentModeKHR::eFifo;
    } else if (requested_mode == "FifoRelaxed") {
        // bbport: the launcher's "FIFO Relaxed": V-sync, but a late frame is shown at once.
        present_mode = vk::PresentModeKHR::eFifoRelaxed;
    } else if (requested_mode == "Immediate") {
        present_mode = vk::PresentModeKHR::eImmediate;
    } else {
        LOG_ERROR(Render_Vulkan, "Unknown present mode {}, defaulting to Mailbox.",
                  EmulatorSettings.GetPresentMode());
        present_mode = vk::PresentModeKHR::eMailbox;
    }

    if (std::ranges::find(modes, present_mode) == modes.cend()) {
        if (window_mode == BbSettings::Borderless &&
            std::ranges::find(modes, vk::PresentModeKHR::eMailbox) != modes.cend()) {
            present_mode = vk::PresentModeKHR::eMailbox;
            return;
        }
        // FIFO is guaranteed to be supported by the Vulkan spec.
        constexpr auto fallback = vk::PresentModeKHR::eFifo;
        LOG_WARNING(Render, "Requested present mode {} is not supported, falling back to {}.",
                    vk::to_string(present_mode), vk::to_string(fallback));
        present_mode = fallback;
    }
}

void Swapchain::SetSurfaceProperties() {
    auto [capabilities_result, capabilities] =
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
#ifdef _WIN32
    if (window_mode == BbSettings::Borderless && instance.HasFullScreenExclusivePolicy()) {
        const VkSurfaceFullScreenExclusiveInfoEXT policy = {
            VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT, nullptr,
            VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT,
        };
        const auto [result, properties] = instance.GetPhysicalDevice().getSurfaceCapabilities2KHR(
            {.pNext = &policy, .surface = surface});
        capabilities_result = result;
        capabilities = properties.surfaceCapabilities;
    }
#endif
    ASSERT_MSG(capabilities_result == vk::Result::eSuccess,
               "Failed to query surface capabilities: {}", vk::to_string(capabilities_result));

    extent = capabilities.currentExtent;
    if (capabilities.currentExtent.width == std::numeric_limits<u32>::max()) {
        extent.width = std::max(capabilities.minImageExtent.width,
                                std::min(capabilities.maxImageExtent.width, width));
        extent.height = std::max(capabilities.minImageExtent.height,
                                 std::min(capabilities.maxImageExtent.height, height));
    }

    // Select number of images in swap chain, we prefer one buffer in the background to work on
    image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    // Prefer identity transform if possible
    transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    if (!(capabilities.supportedTransforms & transform)) {
        transform = capabilities.currentTransform;
    }

    // Opaque is not supported everywhere.
    composite_alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    if (!(capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)) {
        composite_alpha = vk::CompositeAlphaFlagBitsKHR::eInherit;
    }
}

void Swapchain::WaitAcquired() {
    if (!fence_pending) {
        return;
    }
    const vk::Device device = instance.GetDevice();
    Check(device.waitForFences(acquire_fence, true, std::numeric_limits<u64>::max()));
    Check(device.resetFences(acquire_fence));
    fence_pending = false;
}

void Swapchain::Destroy() {
    BbNvidia::BeforeSwapchainChange();
    vk::Device device = instance.GetDevice();
    const auto wait_result = device.waitIdle();
    if (wait_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for device to become idle: {}",
                    vk::to_string(wait_result));
    }
    // bbport: an acquisition's fence that the old swapchain may never signal is not reused.
    if (acquire_fence) {
        if (fence_pending) {
            (void)device.waitForFences(acquire_fence, true, 100'000'000);
        }
        device.destroyFence(acquire_fence);
        acquire_fence = vk::Fence{};
    }
    fence_pending = false;
    acquired = acquired_fenced = false;

    for (auto& image_view : images_view) {
        device.destroyImageView(image_view);
    }
    images_view.clear();

    if (swapchain) {
        device.destroySwapchainKHR(swapchain);
    }

    for (const auto& sem : image_acquired) {
        device.destroySemaphore(sem);
    }
    for (const auto& sem : present_ready) {
        device.destroySemaphore(sem);
    }

    image_acquired.clear();
    present_ready.clear();
}

void Swapchain::RefreshSemaphores() {
    const vk::Device device = instance.GetDevice();
    // bbport: an acquire semaphore is free again once the copy that waited on it has finished.
    // With frame generation two presents share a frame and the frame (present_done) comes back
    // three frames later: one per image ran out while those copies could still be pending.
    image_acquired.resize(image_count * 2 + 2);
    present_ready.resize(image_count);
    frame_index = 0;

    for (vk::Semaphore& semaphore : image_acquired) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create image acquired semaphore: {}",
                   vk::to_string(semaphore_result));
        semaphore = sem;
    }
    for (vk::Semaphore& semaphore : present_ready) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create present ready semaphore: {}", vk::to_string(semaphore_result));
        semaphore = sem;
    }

    for (u32 i = 0; i < image_acquired.size(); ++i) {
        SetObjectName(device, image_acquired[i], "Swapchain Semaphore: image_acquired {}", i);
    }
    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, present_ready[i], "Swapchain Semaphore: present_ready {}", i);
    }
}

void Swapchain::SetupImages() {
    vk::Device device = instance.GetDevice();
    auto [images_result, imgs] = device.getSwapchainImagesKHR(swapchain);
    ASSERT_MSG(images_result == vk::Result::eSuccess, "Failed to create swapchain images: {}",
               vk::to_string(images_result));
    images = std::move(imgs);
    image_count = static_cast<u32>(images.size());
    images_view.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        if (images_view[i]) {
            device.destroyImageView(images_view[i]);
        }
        auto [im_view_result, im_view] = device.createImageView(vk::ImageViewCreateInfo{
            .image = images[i],
            .viewType = vk::ImageViewType::e2D,
            .format = needs_hdr ? SURFACE_FORMAT_HDR.format : surface_format.format,
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        });
        ASSERT_MSG(im_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
                   vk::to_string(im_view_result));
        images_view[i] = im_view;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, images[i], "Swapchain Image {}", i);
        SetObjectName(device, images_view[i], "Swapchain ImageView {}", i);
    }
}

} // namespace Vulkan
