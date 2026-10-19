#pragma once

#include "render/gpu/video_memory.h"
#include "render/vulkan/owned_handle.h"

#include "vk-bootstrap/VkBootstrap.h"
#include "vma/vk_mem_alloc.h"
#include "volk/volk.h"

#include <SDL3/SDL.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

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
    // window may be null for work that never presents (the GPU tests): no surface, and getSurface returns null.
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

    // Whether the driver reports each memory heap's use and budget (VK_EXT_memory_budget, on when the GPU has it).
    bool reportsMemoryBudget() const;

    // Each memory heap's numbers from VMA (see render/gpu/video_memory.h). The driver's figures are fetched again when
    // startMemoryFrame is called, once a frame.
    std::vector<HeapMemory> getHeapMemory() const;
    void startMemoryFrame(std::uint32_t frameNumber) const;

    // Whether images of this format, laid out for the GPU (optimal tiling), support all of these features.
    bool supportsFormat(VkFormat format, VkFormatFeatureFlags2 features) const;

    // The queue's timestamp counter: how many of its bits count before it rolls over (0: it writes no timestamps), and
    // nanoseconds per tick.
    std::uint32_t getTimestampValidBits() const;
    double getTimestampPeriod() const;

    // The most textures one bindless array may hold (combined image samplers in an update-after-bind set, for the
    // fragment stage): the smallest of the limits that apply.
    std::uint32_t getBindlessTextureLimit() const;

    // Debug labels and object names (VK_EXT_debug_utils) for RenderDoc and Nsight. Do nothing when the extension is missing.
    void beginLabel(VkCommandBuffer commands, const char* name) const;
    void endLabel(VkCommandBuffer commands) const;
    void nameObject(VkObjectType type, std::uint64_t handle, const char* name) const;

    // A query pool of count timestamps.
    OwnedHandle<VkQueryPool> createTimestampPool(std::uint32_t count) const;

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
    bool memoryBudget = false;
    bool debugUtils = false;
    std::uint32_t timestampValidBits = 0;
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
