// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Frontend {
class WindowSDL;
}

namespace Vulkan {

class Instance;
class Scheduler;

class Swapchain {
public:
    explicit Swapchain(const Instance& instance, const Frontend::WindowSDL& window);
    ~Swapchain();

    /// Creates (or recreates) the swapchain with a given size.
    void Create(u32 width, u32 height);

    /// Recreates the swapchain with a given size and current surface.
    void Recreate(u32 width, u32 height);

    /// Acquires the next image in the swapchain (bbport: or keeps the one acquired already
    /// and not presented yet: frame generation acquires ahead, off its pacing path).
    /// bbport: fenced: the image's availability is a fence the CPU waits for (WaitAcquired)
    /// instead of a semaphore a GPU submission waits on. Frame generation acquires before an
    /// image is free; a queue left waiting on such a semaphore when the process ends (std::_Exit
    /// ends the driver's present thread, which signals it) never finishes, and the process
    /// cannot end (a killed test run kept 1.7 GB of VRAM).
    bool AcquireNextImage(bool fenced = false);

    /// bbport: waits until a fenced acquisition's image is available (once).
    void WaitAcquired();

    /// Presents the current image and move to the next one
    /// (bbport: on `queue`, by default the instance's present queue).
    bool Present(vk::Queue queue = {});

    /// bbport: whether the instance's compute queue can present to this surface; the
    /// swapchain images are then shared with its family.
    bool ComputePresent() const {
        return compute_present;
    }

    vk::SurfaceKHR GetSurface() const {
        return surface;
    }

    vk::Image Image() const {
        return images[image_index];
    }

    vk::ImageView ImageView() const {
        return images_view[image_index];
    }

    vk::SurfaceFormatKHR GetSurfaceFormat() const {
        return surface_format;
    }

    vk::SwapchainKHR GetHandle() const {
        return swapchain;
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    u32 GetImageCount() const {
        return image_count;
    }

    u32 GetFrameIndex() const {
        return frame_index;
    }

    vk::Extent2D GetExtent() const {
        return extent;
    }

    /// Null when the image was acquired with the fence (WaitAcquired instead).
    [[nodiscard]] vk::Semaphore GetImageAcquiredSemaphore() const {
        return acquired_fenced ? vk::Semaphore{} : image_acquired[frame_index];
    }

    [[nodiscard]] vk::Semaphore GetPresentReadySemaphore() const {
        return present_ready[image_index];
    }

    bool HasHDR() const {
        return supports_hdr;
    }

    void SetHDR(bool hdr);

    bool GetHDR() const {
        return needs_hdr;
    }
    bool WindowPolicyChanged() const;
    /// bbport: the window's surface has a size (0x0 while minimized on Windows).
    bool SurfaceHasArea() const;

private:
    /// Selects the best available swapchain image format
    void FindPresentFormat();

    /// Selects the best available present mode
    void FindPresentMode();

    /// Sets the surface properties according to device capabilities
    void SetSurfaceProperties();

    /// Destroys current swapchain resources
    void Destroy();

    /// Performs creation of image views and framebuffers from the swapchain images
    void SetupImages();

    /// Creates the image acquired and present ready semaphores
    void RefreshSemaphores();

private:
    const Instance& instance;
    const Frontend::WindowSDL& window;
    vk::SwapchainKHR swapchain{};
    vk::SurfaceKHR surface{};
    vk::SurfaceFormatKHR surface_format;
    vk::Format view_format;
    vk::PresentModeKHR present_mode;
    vk::Extent2D extent;
    vk::SurfaceTransformFlagBitsKHR transform;
    vk::CompositeAlphaFlagBitsKHR composite_alpha;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> images_view;
    std::vector<vk::Semaphore> image_acquired;
    std::vector<vk::Semaphore> present_ready;
    u32 width = 0;
    u32 height = 0;
    u32 image_count = 0;
    u32 image_index = 0;
    u32 frame_index = 0;
    bool needs_recreation = true;
    bool compute_present = false;
    bool acquired = false; ///< bbport: image_index is acquired and not presented yet
    bool acquired_fenced = false;  ///< ... with acquire_fence
    bool fence_pending = false;    ///< acquire_fence may still signal: wait before reuse
    vk::Fence acquire_fence{};
    bool reported_empty_surface = false;
    bool needs_hdr = false;    // The game requested HDR swapchain
    bool supports_hdr = false; // SC supports HDR output
    int window_mode = -1;
    bool borderless_vsync = false;
};

} // namespace Vulkan
