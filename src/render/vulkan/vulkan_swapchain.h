#pragma once

#include "render/present_mode.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/owned_handle.h"

#include "volk/volk.h"

#include <SDL3/SDL.h>
#include <cstdint>
#include <optional>
#include <vector>

class VulkanDevice;

/*
* The swapchain: a few images owned by the presentation engine (Windows' compositor), lent to the renderer one at a time.
* Acquiring one gives its index and signals a semaphore when it is really free; the frame draws into it, then presenting
* hands it back once a second semaphore says the drawing is done. It must be rebuilt when the window changes size, when
* HDR output turns on or off (a different format) and when the present mode changes. The device must outlive it.
*/
class VulkanSwapchain {
public:
    VulkanSwapchain(const VulkanDevice& device, SDL_Window* window);
    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    /*
    * Build (or rebuild) the swapchain at the window's size, for HDR (scRGB) or SDR, with this present mode, which must be
    * supported. Wait for the GPU first: the old images may still be in use. Returns false, with no swapchain, while
    * the window has no area (minimised). Throws if the surface offers no fitting format.
    */
    bool rebuild(bool hdr, PresentMode mode, std::uint32_t framesInFlight);

    // Whether the swapchain must be rebuilt before the next frame: it went out of date or the window's size changed.
    bool needsRebuild() const;
    bool exists() const;

    // What the surface can do, asked of the driver (not cheap: call when something changes, not every frame).
    bool supportsHdr() const;
    bool supportsPresentMode(PresentMode mode) const;

    // The next image's index; acquired is signalled once the image is free. None when the swapchain is out of date.
    std::optional<std::uint32_t> acquire(VkSemaphore acquired);

    // Hand the image back to be shown once its renderFinished semaphore is signalled.
    void present(VkQueue queue, std::uint32_t index);

    VkFormat getFormat() const;
    VkExtent2D getExtent() const;
    bool isHdr() const;
    PresentMode getPresentMode() const;
    VkImage getImage(std::uint32_t index) const;
    VkImageView getView(std::uint32_t index) const;
    ImageState& getState(std::uint32_t index);

    // Signalled by the frame that drew into this image, waited for by presentation. One per image, since a semaphore
    // still waited for by an earlier present of another image must not be signalled again.
    VkSemaphore getRenderFinished(std::uint32_t index) const;

private:
    const VulkanDevice& device;
    SDL_Window* window = nullptr;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<OwnedHandle<VkImageView>> views;
    std::vector<ImageState> states;
    std::vector<OwnedHandle<VkSemaphore>> renderFinished;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    bool hdr = false;
    PresentMode presentMode = PresentMode::Vsync;
    bool outOfDate = false;

    void destroyImages() noexcept;
    void destroy() noexcept;
};
