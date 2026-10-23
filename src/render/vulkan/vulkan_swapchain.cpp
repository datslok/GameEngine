#include "render/vulkan/vulkan_swapchain.h"
#include "render/vulkan/swapchain_choice.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {
    std::vector<VkSurfaceFormatKHR> surfaceFormats(const VulkanDevice& device) {
        std::uint32_t count = 0;
        vulkanCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(device.getPhysicalDevice(), device.getSurface(), &count, nullptr),
                    "Surface format query failed");
        std::vector<VkSurfaceFormatKHR> formats(count);
        vulkanCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(device.getPhysicalDevice(), device.getSurface(), &count, formats.data()),
                    "Surface format query failed");
        formats.resize(count);
        return formats;
    }

    std::vector<VkPresentModeKHR> presentModes(const VulkanDevice& device) {
        std::uint32_t count = 0;
        vulkanCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(device.getPhysicalDevice(), device.getSurface(), &count, nullptr),
                    "Present mode query failed");
        std::vector<VkPresentModeKHR> modes(count);
        vulkanCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(device.getPhysicalDevice(), device.getSurface(), &count, modes.data()),
                    "Present mode query failed");
        modes.resize(count);
        return modes;
    }

    VkExtent2D windowPixels(SDL_Window* window) {
        int width = 0;
        int height = 0;

        if (!SDL_GetWindowSizeInPixels(window, &width, &height) || width <= 0 || height <= 0) {
            return {0, 0};
        }

        return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    }
}

VulkanSwapchain::VulkanSwapchain(const VulkanDevice& device, SDL_Window* window):
    device(device),
    window(window)
{
}

VulkanSwapchain::~VulkanSwapchain() {
    destroy();
}

/*
* The old swapchain is passed to the new one, which lets the driver hand its resources over, then destroyed. Images
* are drawn into (colour attachments) and may be copied into; the window is opaque. The present engine may rotate or
* scale (currentTransform); on a desktop that is always the identity.
*/
bool VulkanSwapchain::rebuild(bool wantHdr, PresentMode mode, std::uint32_t framesInFlight) {
    VkSurfaceCapabilitiesKHR capabilities{};
    vulkanCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.getPhysicalDevice(), device.getSurface(), &capabilities),
                "Surface capability query failed");

    const VkExtent2D windowSize = windowPixels(window);
    const VkExtent2D size = chooseSwapchainExtent(capabilities, windowSize.width, windowSize.height);

    if (size.width == 0 || size.height == 0) {
        destroy();
        return false;
    }

    const std::vector<VkSurfaceFormatKHR> formats = surfaceFormats(device);
    const std::optional<VkSurfaceFormatKHR> surfaceFormat = chooseSurfaceFormat(formats, wantHdr);

    if (!surfaceFormat) {
        throw std::runtime_error(wantHdr ? "The window cannot show scRGB (HDR) images" : "The window cannot show sRGB images");
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = device.getSurface();
    info.minImageCount = chooseSwapchainImageCount(capabilities, framesInFlight, mode);
    info.imageFormat = surfaceFormat->format;
    info.imageColorSpace = surfaceFormat->colorSpace;
    info.imageExtent = size;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
        ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
        : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = vulkanPresentMode(mode);
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain;

    VkSwapchainKHR created = VK_NULL_HANDLE;
    vulkanCheck(vkCreateSwapchainKHR(device.getDevice(), &info, nullptr, &created), "Swapchain creation failed");

    destroy();
    swapchain = created;
    format = surfaceFormat->format;
    extent = size;
    hdr = wantHdr;
    presentMode = mode;
    outOfDate = false;

    std::uint32_t count = 0;
    vulkanCheck(vkGetSwapchainImagesKHR(device.getDevice(), swapchain, &count, nullptr), "Swapchain image query failed");
    images.resize(count);
    vulkanCheck(vkGetSwapchainImagesKHR(device.getDevice(), swapchain, &count, images.data()), "Swapchain image query failed");

    for (VkImage image : images) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        vulkanCheck(vkCreateImageView(device.getDevice(), &viewInfo, nullptr, &view), "Swapchain image view creation failed");
        views.emplace_back(device.getDevice(), view, vkDestroyImageView);
        states.push_back(ImageState::Acquired);
        renderFinished.push_back(device.createSemaphore());
    }

    return true;
}

bool VulkanSwapchain::needsRebuild() const {
    if (swapchain == VK_NULL_HANDLE || outOfDate) {
        return true;
    }

    const VkExtent2D windowSize = windowPixels(window);
    return windowSize.width != extent.width || windowSize.height != extent.height;
}

bool VulkanSwapchain::exists() const {
    return swapchain != VK_NULL_HANDLE;
}

bool VulkanSwapchain::supportsHdr() const {
    return device.hasSwapchainColourSpaces() && chooseSurfaceFormat(surfaceFormats(device), true).has_value();
}

bool VulkanSwapchain::supportsPresentMode(PresentMode mode) const {
    return choosePresentMode(mode, presentModes(device)) == mode;
}

/*
* Out of date: the surface changed (a resize) and this swapchain can no longer present to it; nothing was acquired.
* Suboptimal: an image was acquired and can be used, but the swapchain should be rebuilt after this frame.
*/
std::optional<std::uint32_t> VulkanSwapchain::acquire(VkSemaphore acquired) {
    std::uint32_t index = 0;
    const VkResult result = vkAcquireNextImageKHR(device.getDevice(), swapchain, std::numeric_limits<std::uint64_t>::max(),
                                                  acquired, VK_NULL_HANDLE, &index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        outOfDate = true;
        return std::nullopt;
    }

    if (result == VK_SUBOPTIMAL_KHR) {
        outOfDate = true;
    }
    else {
        vulkanCheck(result, "Swapchain image acquisition failed");
    }

    // Whatever the image held is gone: it starts again from the semaphore.
    states[index] = ImageState::Acquired;
    return index;
}

void VulkanSwapchain::present(VkQueue queue, std::uint32_t index) {
    const VkSemaphore wait = renderFinished[index].get();

    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &wait;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain;
    info.pImageIndices = &index;

    const VkResult result = (presentFunction != nullptr ? presentFunction : vkQueuePresentKHR)(queue, &info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        outOfDate = true;
        return;
    }

    vulkanCheck(result, "Presentation failed");
}

void VulkanSwapchain::setPresentFunction(PFN_vkQueuePresentKHR function) {
    presentFunction = function;
}

VkFormat VulkanSwapchain::getFormat() const {
    return format;
}

VkExtent2D VulkanSwapchain::getExtent() const {
    return extent;
}

bool VulkanSwapchain::isHdr() const {
    return hdr;
}

PresentMode VulkanSwapchain::getPresentMode() const {
    return presentMode;
}

VkImage VulkanSwapchain::getImage(std::uint32_t index) const {
    return images.at(index);
}

VkImageView VulkanSwapchain::getView(std::uint32_t index) const {
    return views.at(index).get();
}

ImageState& VulkanSwapchain::getState(std::uint32_t index) {
    return states.at(index);
}

VkSemaphore VulkanSwapchain::getRenderFinished(std::uint32_t index) const {
    return renderFinished.at(index).get();
}

// The images belong to the swapchain; only the views and semaphores made for them are ours.
void VulkanSwapchain::destroyImages() noexcept {
    views.clear();
    renderFinished.clear();
    states.clear();
    images.clear();
}

void VulkanSwapchain::destroy() noexcept {
    destroyImages();

    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device.getDevice(), swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    extent = {0, 0};
}
