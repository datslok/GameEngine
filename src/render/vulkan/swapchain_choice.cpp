#include "render/vulkan/swapchain_choice.h"

#include <algorithm>
#include <array>
#include <limits>

namespace {
    bool offers(std::span<const VkSurfaceFormatKHR> available, VkSurfaceFormatKHR wanted) {
        return std::any_of(available.begin(), available.end(), [&](const VkSurfaceFormatKHR& format) {
            return format.format == wanted.format && format.colorSpace == wanted.colorSpace;
        });
    }
}

/*
* The same preferences as SDL_GPU's SDR_LINEAR and HDR_EXTENDED_LINEAR compositions, so the screen gets exactly what it
* got before the port. The first offered format in the list wins.
*/
std::optional<VkSurfaceFormatKHR> chooseSurfaceFormat(std::span<const VkSurfaceFormatKHR> available, bool hdr) {
    constexpr std::array<VkSurfaceFormatKHR, 2> sdrFormats{{
        {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    }};
    constexpr VkSurfaceFormatKHR scrgb{VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT};

    if (hdr) {
        return offers(available, scrgb) ? std::optional<VkSurfaceFormatKHR>{scrgb} : std::nullopt;
    }

    for (const VkSurfaceFormatKHR& wanted : sdrFormats) {
        if (offers(available, wanted)) {
            return wanted;
        }
    }

    return std::nullopt;
}

PresentMode choosePresentMode(PresentMode requested, std::span<const VkPresentModeKHR> available) {
    const VkPresentModeKHR wanted = vulkanPresentMode(requested);
    const bool offered = std::find(available.begin(), available.end(), wanted) != available.end();
    return offered ? requested : PresentMode::Vsync;
}

VkPresentModeKHR vulkanPresentMode(PresentMode mode) {
    switch (mode) {
    case PresentMode::Mailbox:   return VK_PRESENT_MODE_MAILBOX_KHR;
    case PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case PresentMode::Vsync:     return VK_PRESENT_MODE_FIFO_KHR;
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

/*
* Fewer images means less latency (each queued image is a frame drawn from older input), so the count is the least that
* works. Mailbox needs a spare: with only two, the waiting image could never be replaced while the other is on screen.
*/
std::uint32_t chooseSwapchainImageCount(const VkSurfaceCapabilitiesKHR& capabilities, std::uint32_t framesInFlight,
                                        PresentMode mode) {
    std::uint32_t count = std::max(framesInFlight, capabilities.minImageCount);

    if (mode == PresentMode::Mailbox) {
        count = std::max(count, std::uint32_t{3});
    }

    if (capabilities.maxImageCount > 0) {
        count = std::min(count, capabilities.maxImageCount);
    }

    return count;
}

/*
* Windows always states the size (the window's client area, 0 x 0 while minimised, which the renderer skips). The
* special value of all ones means "whatever the swapchain says", and then the window's size is used.
*/
VkExtent2D chooseSwapchainExtent(const VkSurfaceCapabilitiesKHR& capabilities, std::uint32_t windowWidth, std::uint32_t windowHeight) {
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    return VkExtent2D{
        std::clamp(windowWidth, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        std::clamp(windowHeight, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
    };
}
