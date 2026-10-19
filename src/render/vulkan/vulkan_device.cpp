#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_check.h"

#include <SDL3/SDL_vulkan.h>

#include <stdexcept>
#include <string>

namespace {
    std::runtime_error bootstrapError(const char* message, const std::error_code& error) {
        return std::runtime_error(std::string{message} + ": " + error.message());
    }
}

VulkanDevice::VulkanDevice(SDL_Window* window, bool validation):
    window(window)
{
    try {
        createInstance(validation);
        selectPhysicalDevice();
        createDevice();
        createAllocator();
    }
    catch (...) {
        cleanup();
        throw;
    }
}

VulkanDevice::~VulkanDevice() {
    cleanup();
}

/*
* volk loads the Vulkan library (vulkan-1.dll), takes the one function every other is looked up through
* (vkGetInstanceProcAddr) and fetches the rest straight from the driver, so calls skip the loader's dispatch. It keeps
* the library loaded for the whole run, which matters: vk-bootstrap remembers the library's first functions for the
* life of the process, and SDL unloads its own copy whenever its video system shuts down (between golden images).
* SDL loads the library too, since it makes the window's surface and knows which instance extensions that needs.
* vk-bootstrap builds the instance: Vulkan 1.3, SDL's extensions, the colour space extension when present (for scRGB
* HDR output), and in debug builds the validation layers, whose messages come back through onValidationMessage.
* Without a window (the GPU tests) SDL is left out entirely: no surface, no surface extensions (vk-bootstrap's
* headless mode), so the GPU is chosen without asking whether it can present.
*/
void VulkanDevice::createInstance(bool validation) {
    static const VkResult volkLoaded = volkInitialize();
    vulkanCheck(volkLoaded, "Vulkan library loading failed");

    const auto systemInfo = vkb::SystemInfo::get_system_info(vkGetInstanceProcAddr);

    if (!systemInfo) {
        throw bootstrapError("Vulkan system query failed", systemInfo.error());
    }

    vkb::InstanceBuilder builder{vkGetInstanceProcAddr};
    builder.set_app_name("GameEngine")
        .require_api_version(1, 3, 0)
        .set_headless(window == nullptr);

    if (window != nullptr) {
        if (!SDL_Vulkan_LoadLibrary(nullptr)) {
            throw std::runtime_error(std::string{"Vulkan library loading failed: "} + SDL_GetError());
        }

        sdlVulkanLoaded = true;

        Uint32 sdlExtensionCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);

        if (sdlExtensions == nullptr) {
            throw std::runtime_error(std::string{"Vulkan surface extensions unavailable: "} + SDL_GetError());
        }

        builder.enable_extensions(sdlExtensionCount, sdlExtensions);
    }

    swapchainColourSpaces = window != nullptr && systemInfo->is_extension_available(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);

    if (swapchainColourSpaces) {
        builder.enable_extension(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }

    if (validation) {
        if (systemInfo->validation_layers_available && systemInfo->debug_utils_available) {
            builder.request_validation_layers(true)
                .set_debug_callback(&VulkanDevice::onValidationMessage)
                .set_debug_callback_user_data_pointer(this)
                .set_debug_messenger_severity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                .set_debug_messenger_type(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT);
        }
        else {
            SDL_Log("Vulkan validation layers not found, running without them (MSYS2 installs them in C:\\msys64\\ucrt64\\bin: "
                    "set VK_ADD_LAYER_PATH to that folder, as make run does)");
        }
    }

    // Debug labels and names cost nothing without a tool listening, so they are on in release builds too (profiling
    // with Nsight wants optimised code). With validation, vk-bootstrap already adds the extension for its messages.
    const bool validationMessages = validation && systemInfo->validation_layers_available && systemInfo->debug_utils_available;

    if (systemInfo->debug_utils_available && !validationMessages) {
        builder.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    auto built = builder.build();

    if (!built) {
        throw bootstrapError("Vulkan instance creation failed", built.error());
    }

    instance = built.value();
    volkLoadInstanceOnly(instance.instance);
    debugUtils = systemInfo->debug_utils_available && vkCmdBeginDebugUtilsLabelEXT != nullptr;

    if (window != nullptr && !SDL_Vulkan_CreateSurface(window, instance.instance, nullptr, &surface)) {
        throw std::runtime_error(std::string{"Vulkan surface creation failed: "} + SDL_GetError());
    }
}

/*
* A GPU qualifies if it runs Vulkan 1.3, can present to the window (when there is one), and has the features the renderer uses: dynamic
* rendering (draw into images named when a pass begins, no render pass objects), synchronization2 (the clearer barrier
* API), anisotropic filtering, and depth clamping (SDL_GPU's default, kept so the pictures match: depths past the near
* and far planes are clamped instead of clipped). A discrete GPU is preferred over an integrated one.
*/
void VulkanDevice::selectPhysicalDevice() {
    VkPhysicalDeviceFeatures features{};
    features.samplerAnisotropy = VK_TRUE;
    features.depthClamp = VK_TRUE;

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    vkb::PhysicalDeviceSelector selector{instance, surface};
    auto selected = selector.set_minimum_version(1, 3)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
        .set_required_features(features)
        .set_required_features_13(features13)
        .select();

    if (!selected) {
        throw bootstrapError("No GPU supports what the Vulkan renderer needs", selected.error());
    }

    physicalDevice = selected.value();
}

/*
* One queue does everything: drawing, copies and presenting. Separate copy or compute queues can come later, when
* there is work worth overlapping.
*/
void VulkanDevice::createDevice() {
    vkb::DeviceBuilder builder{physicalDevice};
    auto built = builder.build();

    if (!built) {
        throw bootstrapError("Vulkan device creation failed", built.error());
    }

    device = built.value();
    volkLoadDevice(device.device);

    const auto graphics = device.get_queue_and_index(vkb::QueueType::graphics);

    if (!graphics) {
        throw bootstrapError("The GPU has no drawing queue", graphics.error());
    }

    queue = graphics->first;
    queueFamily = graphics->second;

    if (surface != VK_NULL_HANDLE) {
        VkBool32 presents = VK_FALSE;
        vulkanCheck(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice.physical_device, queueFamily, surface, &presents),
                    "Present support query failed");

        if (presents != VK_TRUE) {
            throw std::runtime_error("The GPU's drawing queue cannot present to the window");
        }
    }

    timestampValidBits = device.queue_families.at(queueFamily).timestampValidBits;
}

// VMA finds Vulkan's functions through the two lookups volk already loaded.
void VulkanDevice::createAllocator() {
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.physicalDevice = physicalDevice.physical_device;
    info.device = device.device;
    info.instance = instance.instance;
    info.pVulkanFunctions = &functions;

    vulkanCheck(vmaCreateAllocator(&info, &allocator), "Memory allocator creation failed");
}

// In reverse order of creation: everything made from the device first, the instance last.
void VulkanDevice::cleanup() noexcept {
    if (allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator);
        allocator = VK_NULL_HANDLE;
    }

    if (device.device != VK_NULL_HANDLE) {
        vkb::destroy_device(device);
        device = vkb::Device{};
    }

    if (surface != VK_NULL_HANDLE) {
        vkb::destroy_surface(instance, surface);
        surface = VK_NULL_HANDLE;
    }

    if (instance.instance != VK_NULL_HANDLE) {
        vkb::destroy_instance(instance);
        instance = vkb::Instance{};
    }

    if (sdlVulkanLoaded) {
        SDL_Vulkan_UnloadLibrary();
        sdlVulkanLoaded = false;
    }
}

/*
* Errors break the spec (the GPU may do anything), so they are counted for the golden image check; warnings are
* usually performance advice. Both are logged with the layers' own message, which names the rule that was broken.
*/
VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::onValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                                 VkDebugUtilsMessageTypeFlagsEXT types,
                                                                 const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                                 void* userData) {
    const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    const bool fromLayers = (types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) != 0;

    // The loader's own warnings (registry entries it could not read, duplicate layers) are about the PC, not the renderer.
    if (!error && !fromLayers) {
        return VK_FALSE;
    }

    if (error && userData != nullptr) {
        static_cast<VulkanDevice*>(userData)->validationErrors.fetch_add(1);
    }

    SDL_Log("Vulkan validation %s: %s", error ? "error" : "warning", data != nullptr ? data->pMessage : "");
    return VK_FALSE;
}

VkInstance VulkanDevice::getInstance() const {
    return instance.instance;
}

VkPhysicalDevice VulkanDevice::getPhysicalDevice() const {
    return physicalDevice.physical_device;
}

VkDevice VulkanDevice::getDevice() const {
    return device.device;
}

VkSurfaceKHR VulkanDevice::getSurface() const {
    return surface;
}

VkQueue VulkanDevice::getQueue() const {
    return queue;
}

std::uint32_t VulkanDevice::getQueueFamily() const {
    return queueFamily;
}

VmaAllocator VulkanDevice::getAllocator() const {
    return allocator;
}

const VkPhysicalDeviceLimits& VulkanDevice::getLimits() const {
    return physicalDevice.properties.limits;
}

const std::string& VulkanDevice::getName() const {
    return physicalDevice.name;
}

bool VulkanDevice::hasSwapchainColourSpaces() const {
    return swapchainColourSpaces;
}

bool VulkanDevice::supportsFormat(VkFormat format, VkFormatFeatureFlags2 features) const {
    VkFormatProperties3 properties3{};
    properties3.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;

    VkFormatProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    properties.pNext = &properties3;

    vkGetPhysicalDeviceFormatProperties2(physicalDevice.physical_device, format, &properties);
    return (properties3.optimalTilingFeatures & features) == features;
}

std::uint32_t VulkanDevice::getTimestampValidBits() const {
    return timestampValidBits;
}

double VulkanDevice::getTimestampPeriod() const {
    return physicalDevice.properties.limits.timestampPeriod;
}

/*
* A label marks a stretch of a command buffer ("Shadows", "Bloom"), so RenderDoc and Nsight show the frame as named
* passes instead of a flat list of draws. Labels nest, and each begin needs its end in the same command buffer.
*/
void VulkanDevice::beginLabel(VkCommandBuffer commands, const char* name) const {
    if (!debugUtils) {
        return;
    }

    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    vkCmdBeginDebugUtilsLabelEXT(commands, &label);
}

void VulkanDevice::endLabel(VkCommandBuffer commands) const {
    if (debugUtils) {
        vkCmdEndDebugUtilsLabelEXT(commands);
    }
}

// Named objects show up by name in the tools and in validation messages, instead of as bare handle numbers.
void VulkanDevice::nameObject(VkObjectType type, std::uint64_t handle, const char* name) const {
    if (!debugUtils || handle == 0) {
        return;
    }

    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    vulkanCheck(vkSetDebugUtilsObjectNameEXT(device.device, &info), "Naming a Vulkan object failed");
}

OwnedHandle<VkQueryPool> VulkanDevice::createTimestampPool(std::uint32_t count) const {
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = count;

    VkQueryPool pool = VK_NULL_HANDLE;
    vulkanCheck(vkCreateQueryPool(device.device, &info, nullptr, &pool), "Timestamp query pool creation failed");
    return {device.device, pool, vkDestroyQueryPool};
}

std::uint32_t VulkanDevice::getValidationErrorCount() const {
    return validationErrors.load();
}

void VulkanDevice::waitIdle() const noexcept {
    if (device.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device.device);
    }
}

OwnedHandle<VkSemaphore> VulkanDevice::createSemaphore() const {
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkSemaphore semaphore = VK_NULL_HANDLE;
    vulkanCheck(vkCreateSemaphore(device.device, &info, nullptr, &semaphore), "Semaphore creation failed");
    return {device.device, semaphore, vkDestroySemaphore};
}

// A fence created signalled lets the first wait on it pass at once, as if a previous frame had finished.
OwnedHandle<VkFence> VulkanDevice::createFence(bool signalled) const {
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    info.flags = signalled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;

    VkFence fence = VK_NULL_HANDLE;
    vulkanCheck(vkCreateFence(device.device, &info, nullptr, &fence), "Fence creation failed");
    return {device.device, fence, vkDestroyFence};
}

OwnedHandle<VkSampler> VulkanDevice::createSampler(const VkSamplerCreateInfo& info) const {
    VkSampler sampler = VK_NULL_HANDLE;
    vulkanCheck(vkCreateSampler(device.device, &info, nullptr, &sampler), "Sampler creation failed");
    return {device.device, sampler, vkDestroySampler};
}

// Command buffers from this pool are reset as a whole each time they are reused.
OwnedHandle<VkCommandPool> VulkanDevice::createCommandPool() const {
    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info.queueFamilyIndex = queueFamily;

    VkCommandPool pool = VK_NULL_HANDLE;
    vulkanCheck(vkCreateCommandPool(device.device, &info, nullptr, &pool), "Command pool creation failed");
    return {device.device, pool, vkDestroyCommandPool};
}

// A compiled SPIR-V file (glslc's output), whole: SDL_LoadFile's buffer is aligned for the 32-bit words Vulkan reads.
OwnedHandle<VkShaderModule> VulkanDevice::loadShader(const char* path) const {
    std::size_t codeSize = 0;
    void* code = SDL_LoadFile(path, &codeSize);

    if (code == nullptr) {
        throw std::runtime_error(std::string{path} + ": " + SDL_GetError());
    }

    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = codeSize;
    info.pCode = static_cast<const std::uint32_t*>(code);

    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(device.device, &info, nullptr, &module);
    SDL_free(code);
    vulkanCheck(result, path);
    return {device.device, module, vkDestroyShaderModule};
}
