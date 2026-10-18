#pragma once

#include "render/vulkan/owned_handle.h"

#include "vk-bootstrap/VkBootstrap.h"
#include "vma/vk_mem_alloc.h"
#include "volk/volk.h"

#include <SDL3/SDL.h>
#include <atomic>
#include <cstdint>
#include <string>

/*
* The engine's connection to the GPU through Vulkan 1.3: the instance (Vulkan itself, with the validation layers in
* debug builds), the window's surface, the chosen GPU (the physical device), the logical device made from it with the
* features the renderer uses, one queue that both draws and presents, and the memory allocator (VMA) that carves
* buffers and images out of the GPU's memory heaps. Everything else in the renderer is made from this, so it is
* created first and destroyed last; the window must outlive it.
*/
class VulkanDevice {
public:
    // validation turns on the Khronos validation layers when installed: every call checked against the spec, slow.
    VulkanDevice(SDL_Window* window, bool validation);
    ~VulkanDevice();

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    VkInstance getInstance() const;
    VkPhysicalDevice getPhysicalDevice() const;
    VkDevice getDevice() const;
    VkSurfaceKHR getSurface() const;
    VkQueue getQueue() const;
    std::uint32_t getQueueFamily() const;
    VmaAllocator getAllocator() const;
    const VkPhysicalDeviceLimits& getLimits() const;
    const std::string& getName() const;

    // The instance can describe swapchain colour spaces other than plain sRGB (scRGB, for HDR output).
    bool hasSwapchainColourSpaces() const;

    // Whether images of this format, laid out for the GPU (optimal tiling), support all of these features.
    bool supportsFormat(VkFormat format, VkFormatFeatureFlags2 features) const;

    // Errors the validation layers reported so far (0 without validation).
    std::uint32_t getValidationErrorCount() const;

    // Waits until the GPU has finished everything submitted, before anything it may still use is destroyed.
    void waitIdle() const noexcept;

    // Small objects owned by the caller.
    OwnedHandle<VkSemaphore> createSemaphore() const;
    OwnedHandle<VkFence> createFence(bool signalled) const;
    OwnedHandle<VkSampler> createSampler(const VkSamplerCreateInfo& info) const;
    OwnedHandle<VkCommandPool> createCommandPool() const;
    OwnedHandle<VkShaderModule> loadShader(const char* path) const;

private:
    SDL_Window* window = nullptr;
    vkb::Instance instance;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    bool sdlVulkanLoaded = false;
    vkb::PhysicalDevice physicalDevice;
    vkb::Device device;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    VmaAllocator allocator = VK_NULL_HANDLE;
    bool swapchainColourSpaces = false;
    std::atomic<std::uint32_t> validationErrors{0};

    void createInstance(bool validation);
    void selectPhysicalDevice();
    void createDevice();
    void createAllocator();
    void cleanup() noexcept;

    static VKAPI_ATTR VkBool32 VKAPI_CALL onValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                              VkDebugUtilsMessageTypeFlagsEXT types,
                                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                              void* userData);
};
