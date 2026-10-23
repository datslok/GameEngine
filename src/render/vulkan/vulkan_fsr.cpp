#include "render/vulkan/vulkan_fsr.h"
#include "core/build_info.h"
#include "render/vulkan/vulkan_device.h"

// After volk, which brings Vulkan's types without its prototypes; AMD's headers include vulkan.h themselves.
#include "fidelityfx/ffx_api/ffx_upscale.h"
#include "fidelityfx/ffx_api/vk/ffx_api_vk.h"

#include <SDL3/SDL.h>

#include <stdexcept>
#include <string>

namespace {
    constexpr const char* fsrLibrary = "amd_fidelityfx_vk.dll";

    // FSR's output keeps the scene's format, so upscaling loses none of its range.
    constexpr VkFormat outputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    /*
    * FSR's messages (with its debug checks on, in debug builds) arrive as wide strings. Its messages are English, so
    * each character is kept as it is when it fits a byte, which is enough for the log.
    */
    void logFsrMessage(std::uint32_t type, const wchar_t* message) {
        std::string text;

        for (const wchar_t* character = message; character != nullptr && *character != L'\0'; ++character) {
            text.push_back(*character < 128 ? static_cast<char>(*character) : '?');
        }

        SDL_Log("FSR %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text.c_str());
    }

    /*
    * An image as the FidelityFX API names it: its handle, its description (from the same fields Vulkan was given to
    * make it, which the API's own helper turns into its formats and uses), and the state it is in, which FSR's barriers
    * start from and return it to.
    */
    FfxApiResource fsrResource(const VulkanImage& image, std::uint32_t extraUsage, std::uint32_t state) {
        const ImageDescription& description = image.getDescription();
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = description.format;
        info.extent = {description.width, description.height, 1};
        info.mipLevels = description.levels;
        info.arrayLayers = 1;
        info.usage = description.usage;
        return ffxApiGetResourceVK(reinterpret_cast<void*>(image.get()), ffxApiGetImageResourceDescriptionVK(image.get(), info, extraUsage), state);
    }
}

/*
* The DLL is looked for beside the game (the makefile copies it into the build folders, the packaged demo carries it)
* and then on the PATH. Without it, or without one of its functions, or on a GPU missing the features its shaders use,
* FSR stays unavailable and the game carries on.
*/
VulkanFsr::VulkanFsr(const VulkanDevice& device):
    device(device)
{
    library = SDL_LoadObject(fsrLibrary);

    if (library == nullptr) {
        SDL_Log("FSR 3.1 unavailable: %s", SDL_GetError());
        return;
    }

    createContext = reinterpret_cast<void*>(SDL_LoadFunction(library, "ffxCreateContext"));
    destroyContext = reinterpret_cast<void*>(SDL_LoadFunction(library, "ffxDestroyContext"));
    dispatchFunction = reinterpret_cast<void*>(SDL_LoadFunction(library, "ffxDispatch"));

    if (createContext == nullptr || destroyContext == nullptr || dispatchFunction == nullptr) {
        SDL_Log("FSR 3.1 unavailable: %s lacks the FidelityFX API's functions", fsrLibrary);
        return;
    }

    if (!device.supportsFsrFeatures()) {
        SDL_Log("FSR 3.1 unavailable: the GPU lacks 16-bit shader types or storage image reads without a format");
        return;
    }

    available = true;
}

VulkanFsr::~VulkanFsr() {
    dropContext();

    if (library != nullptr) {
        SDL_UnloadObject(library);
    }
}

bool VulkanFsr::isAvailable() const {
    return available;
}

void VulkanFsr::ensureSize(std::uint32_t outputWidth, std::uint32_t outputHeight) {
    if (width == outputWidth && height == outputHeight) {
        return;
    }

    dropContext();

    ImageDescription description;
    description.format = outputFormat;
    description.width = outputWidth;
    description.height = outputHeight;
    // Storage: FSR writes it; sampled: eye adaptation, bloom and tone mapping read it; transfer source: captures copy it.
    description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    description.name = "FSR output";
    output = VulkanImage{device, description};
    width = outputWidth;
    height = outputHeight;
}

/*
* The context is made for renders up to the output's own size, so a new render scale needs no new context. Its input is
* HDR (the scene in linear light), its depth reversed-Z with a far plane, and it works out its own exposure, as AMD
* recommends; in debug builds it checks what it is given and reports through the log.
*/
bool VulkanFsr::makeContext() {
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = device.getDevice();
    backend.vkPhysicalDevice = device.getPhysicalDevice();
    backend.vkDeviceProcAddr = vkGetDeviceProcAddr;

    ffxCreateContextDescUpscale upscale{};
    upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscale.header.pNext = &backend.header;
    upscale.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;

    if (isDebugBuild) {
        upscale.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    }

    upscale.maxRenderSize = {width, height};
    upscale.maxUpscaleSize = {width, height};
    upscale.fpMessage = logFsrMessage;

    const auto create = reinterpret_cast<PfnFfxCreateContext>(createContext);
    const ffxReturnCode_t result = create(&context, &upscale.header, nullptr);

    if (result != FFX_API_RETURN_OK) {
        SDL_Log("FSR 3.1 unavailable: its context could not be made (FidelityFX API error %u)", result);
        context = nullptr;
        available = false;
        return false;
    }

    return true;
}

void VulkanFsr::dropContext() {
    if (context != nullptr) {
        reinterpret_cast<PfnFfxDestroyContext>(destroyContext)(&context, nullptr);
        context = nullptr;
    }
}

/*
* The states FSR is told the images are in, and leaves them in: the colour, velocity and depth read by compute
* (SHADER_READ_ONLY_OPTIMAL, which a depth image may take too), the output a storage image (GENERAL). After it, the
* output becomes readable for eye adaptation, bloom and tone mapping. Its motion vectors are this frame's place to last
* frame's in render pixels: ours times the inputs' scale.
*/
bool VulkanFsr::dispatch(VkCommandBuffer commands, VulkanImage& colour, VulkanImage& depth, VulkanImage& velocity, const FsrFrameInputs& inputs) {
    if (!available || (context == nullptr && !makeContext())) {
        return false;
    }

    colour.transition(commands, ImageState::ComputeRead);
    depth.transition(commands, ImageState::ComputeRead);
    velocity.transition(commands, ImageState::ComputeRead);
    output.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    ffxDispatchDescUpscale upscale{};
    upscale.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    upscale.commandList = commands;
    upscale.color = fsrResource(colour, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    upscale.depth = fsrResource(depth, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    upscale.motionVectors = fsrResource(velocity, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    upscale.output = fsrResource(output, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    upscale.jitterOffset = {inputs.jitter.x, inputs.jitter.y};
    upscale.motionVectorScale = {inputs.motionVectorScale.x, inputs.motionVectorScale.y};
    upscale.renderSize = {inputs.renderSize.width, inputs.renderSize.height};
    upscale.upscaleSize = {inputs.outputWidth, inputs.outputHeight};
    upscale.enableSharpening = inputs.sharpen;
    upscale.sharpness = inputs.sharpness;
    upscale.frameTimeDelta = inputs.frameMilliseconds;
    upscale.preExposure = 1.0f;
    upscale.reset = inputs.reset;
    upscale.cameraNear = inputs.cameraNear;
    upscale.cameraFar = inputs.cameraFar;
    upscale.cameraFovAngleVertical = inputs.verticalFov;
    upscale.viewSpaceToMetersFactor = 1.0f;

    const ffxReturnCode_t result = reinterpret_cast<PfnFfxDispatch>(dispatchFunction)(&context, &upscale.header);

    if (result != FFX_API_RETURN_OK) {
        throw std::runtime_error("FSR 3.1's dispatch failed (FidelityFX API error " + std::to_string(result) + ")");
    }

    output.transition(commands, ImageState::SurfaceRead);
    return true;
}

VulkanImage& VulkanFsr::getOutput() {
    return output;
}
