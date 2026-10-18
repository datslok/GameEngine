#include "render/vulkan/swapchain_choice.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

namespace {
    bool sameFormat(const VkSurfaceFormatKHR& a, VkFormat format, VkColorSpaceKHR colourSpace) {
        return a.format == format && a.colorSpace == colourSpace;
    }

    /*
    * SDR wants an sRGB format, so the GPU encodes the shader's linear output as it is written: blue-green-red order
    * first (what Windows drivers list), red-green-blue as the fallback, both in the plain sRGB colour space.
    */
    void testSdrPrefersBgrSrgb() {
        const std::vector<VkSurfaceFormatKHR> available{
            {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
            {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
            {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        };

        const auto chosen = chooseSurfaceFormat(available, false);
        assert(chosen.has_value());
        assert(sameFormat(*chosen, VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR));
    }

    void testSdrFallsBackToRgbSrgb() {
        const std::vector<VkSurfaceFormatKHR> available{
            {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
            {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        };

        const auto chosen = chooseSurfaceFormat(available, false);
        assert(chosen.has_value());
        assert(sameFormat(*chosen, VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR));
    }

    // An sRGB format in another colour space (here HDR10's) is not plain SDR, and without any there is no choice.
    void testSdrWithoutSrgbFormatHasNoChoice() {
        const std::vector<VkSurfaceFormatKHR> available{
            {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
            {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_HDR10_ST2084_EXT},
        };

        assert(!chooseSurfaceFormat(available, false).has_value());
    }

    // HDR output is scRGB: 16-bit floats in the extended linear sRGB space, where 1.0 is 80 nits and more is brighter.
    void testHdrNeedsScrgb() {
        const std::vector<VkSurfaceFormatKHR> withScrgb{
            {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
            {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
        };

        const auto chosen = chooseSurfaceFormat(withScrgb, true);
        assert(chosen.has_value());
        assert(sameFormat(*chosen, VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT));

        // Float images in the plain sRGB space would be clipped at white, so they do not count.
        const std::vector<VkSurfaceFormatKHR> withoutScrgb{
            {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
            {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        };

        assert(!chooseSurfaceFormat(withoutScrgb, true).has_value());
    }

    // Every driver must offer first-in-first-out presentation (vsync), so any other mode falls back to it.
    void testPresentModeFallsBackToVsync() {
        const std::vector<VkPresentModeKHR> fifoOnly{VK_PRESENT_MODE_FIFO_KHR};
        assert(choosePresentMode(PresentMode::Mailbox, fifoOnly) == PresentMode::Vsync);
        assert(choosePresentMode(PresentMode::Immediate, fifoOnly) == PresentMode::Vsync);
        assert(choosePresentMode(PresentMode::Vsync, fifoOnly) == PresentMode::Vsync);

        const std::vector<VkPresentModeKHR> all{VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR};
        assert(choosePresentMode(PresentMode::Mailbox, all) == PresentMode::Mailbox);
        assert(choosePresentMode(PresentMode::Immediate, all) == PresentMode::Immediate);
    }

    void testPresentModesMapToVulkan() {
        assert(vulkanPresentMode(PresentMode::Vsync) == VK_PRESENT_MODE_FIFO_KHR);
        assert(vulkanPresentMode(PresentMode::Mailbox) == VK_PRESENT_MODE_MAILBOX_KHR);
        assert(vulkanPresentMode(PresentMode::Immediate) == VK_PRESENT_MODE_IMMEDIATE_KHR);
    }

    VkSurfaceCapabilitiesKHR capabilities(std::uint32_t minImages, std::uint32_t maxImages) {
        VkSurfaceCapabilitiesKHR result{};
        result.minImageCount = minImages;
        result.maxImageCount = maxImages;
        result.currentExtent = {1920, 1080};
        result.minImageExtent = {1, 1};
        result.maxImageExtent = {16384, 16384};
        return result;
    }

    /*
    * One image per frame in flight, but never fewer than the driver's minimum. Mailbox needs three: one on screen,
    * one waiting, and one being drawn, or a newer frame could never replace the waiting one. A maximum of 0 means
    * no maximum.
    */
    void testSwapchainImageCount() {
        assert(chooseSwapchainImageCount(capabilities(2, 8), 1, PresentMode::Vsync) == 2);
        assert(chooseSwapchainImageCount(capabilities(2, 8), 3, PresentMode::Vsync) == 3);
        assert(chooseSwapchainImageCount(capabilities(2, 8), 1, PresentMode::Mailbox) == 3);
        assert(chooseSwapchainImageCount(capabilities(2, 0), 5, PresentMode::Vsync) == 5);
        assert(chooseSwapchainImageCount(capabilities(2, 4), 6, PresentMode::Immediate) == 4);
    }

    /*
    * The surface usually states its size (the window's client area in pixels); a current extent of all ones means
    * the swapchain decides, so the window's size is used, kept within the surface's limits.
    */
    void testSwapchainExtent() {
        const VkSurfaceCapabilitiesKHR fixed = capabilities(2, 8);
        const VkExtent2D fromSurface = chooseSwapchainExtent(fixed, 800, 600);
        assert(fromSurface.width == 1920 && fromSurface.height == 1080);

        VkSurfaceCapabilitiesKHR free = fixed;
        free.currentExtent = {std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max()};
        free.maxImageExtent = {1000, 1000};
        const VkExtent2D fromWindow = chooseSwapchainExtent(free, 800, 1200);
        assert(fromWindow.width == 800 && fromWindow.height == 1000);
    }
}

void testSwapchainChoice() {
    testSdrPrefersBgrSrgb();
    testSdrFallsBackToRgbSrgb();
    testSdrWithoutSrgbFormatHasNoChoice();
    testHdrNeedsScrgb();
    testPresentModeFallsBackToVsync();
    testPresentModesMapToVulkan();
    testSwapchainImageCount();
    testSwapchainExtent();
}
