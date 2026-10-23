#include "render/vulkan/vulkan_dlss.h"

// Windows' headers, which Streamline's signature check needs, without their min and max macros or their rarely used parts.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// After volk, which brings Vulkan's types without its prototypes; Streamline's Vulkan helpers include vulkan.h themselves.
#include "streamline/include/sl.h"
#include "streamline/include/sl_dlss.h"
#include "streamline/include/sl_helpers_vk.h"
#include "streamline/include/sl_security.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

/*
* Streamline's functions, as loaded from sl.interposer.dll by name, and DLSS's own, which Streamline hands out once it
* knows the device.
*/
struct StreamlineFunctions {
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slGetFeatureRequirements* featureRequirements = nullptr;
    PFun_slIsFeatureSupported* featureSupported = nullptr;
    PFun_slSetVulkanInfo* setVulkanInfo = nullptr;
    PFun_slGetFeatureFunction* featureFunction = nullptr;
    PFun_slGetNewFrameToken* newFrameToken = nullptr;
    PFun_slSetConstants* setConstants = nullptr;
    PFun_slSetTagForFrame* setTags = nullptr;
    PFun_slEvaluateFeature* evaluate = nullptr;
    PFun_slFreeResources* freeResources = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
};

// dlssPresetNumber (render/gpu/upscaling.h) numbers the models as Streamline does, without including its headers.
static_assert(static_cast<std::uint32_t>(sl::DLSSPreset::eDefault) == 0 && static_cast<std::uint32_t>(sl::DLSSPreset::ePresetJ) == 10 &&
              static_cast<std::uint32_t>(sl::DLSSPreset::ePresetM) == 13, "DlssModel must match Streamline's DLSSPreset numbers");

namespace {
    // The engine's own id for NVIDIA's records (any fixed GUID identifies a custom engine's project).
    constexpr const char* projectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";

    // Streamline's one viewport: the whole screen.
    constexpr std::uint32_t viewportId = 0;

    /*
    * Streamline's warnings and errors go to the log; its information (a hundred lines at start-up) does not, nor do
    * two kinds of message the engine has already: the validation layers' own, which Streamline repeats through a
    * messenger of its own, and three warnings expected with manual hooking (it cannot watch the engine's pipeline and
    * descriptor binds or command buffers begin, so it cannot restore them after evaluating, which the engine never
    * relies on: every pass binds its own).
    */
    void logStreamlineMessage(sl::LogType type, const char* message) {
        if (type == sl::LogType::eInfo || message == nullptr || std::strstr(message, "is NOT supported, plugin will not function properly") != nullptr ||
            std::strstr(message, "debugUtilsMessengerCallback") != nullptr) {
            return;
        }

        SDL_Log("Streamline %s: %s", type == sl::LogType::eError ? "error" : "warning", message);
    }

    // The folder the game runs from, as Windows' wide characters: the DLLs sit beside the exe.
    std::wstring executableFolder() {
        const char* base = SDL_GetBasePath();
        const std::string folder = base != nullptr ? base : ".\\";
        const int length = MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(std::max(length, 1)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), -1, wide.data(), length);
        wide.resize(wide.size() - 1);
        return wide;
    }

    template <typename Function> Function* loadFunction(SDL_SharedObject* library, const char* name) {
        return reinterpret_cast<Function*>(reinterpret_cast<void*>(SDL_LoadFunction(library, name)));
    }

    /*
    * Loaded once for the whole run and never unloaded: a run may make several renderers (the golden images make one
    * per game), and each starts and stops Streamline on the same library. Null when it is missing or not signed by
    * NVIDIA (a DLL beside the exe could be anyone's), which NVIDIA asks every game to check.
    */
    SDL_SharedObject* interposerLibrary() {
        static SDL_SharedObject* const library = [] {
            const std::wstring path = executableFolder() + L"sl.interposer.dll";

            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                SDL_Log("DLSS unavailable: sl.interposer.dll is not beside the game (run python tools/fetch_streamline.py, then make)");
                return static_cast<SDL_SharedObject*>(nullptr);
            }

            if (!sl::security::verifyEmbeddedSignature(path.c_str())) {
                SDL_Log("DLSS unavailable: sl.interposer.dll is not signed by NVIDIA");
                return static_cast<SDL_SharedObject*>(nullptr);
            }

            SDL_SharedObject* loaded = SDL_LoadObject("sl.interposer.dll");

            if (loaded == nullptr) {
                SDL_Log("DLSS unavailable: %s", SDL_GetError());
            }

            return loaded;
        }();

        return library;
    }

    sl::DLSSMode dlssMode(UpscalingMode mode) {
        switch (mode) {
            case UpscalingMode::Native:
                return sl::DLSSMode::eDLAA;
            case UpscalingMode::Quality:
                return sl::DLSSMode::eMaxQuality;
            case UpscalingMode::Balanced:
                return sl::DLSSMode::eBalanced;
            case UpscalingMode::Performance:
                return sl::DLSSMode::eMaxPerformance;
            case UpscalingMode::UltraPerformance:
                return sl::DLSSMode::eUltraPerformance;
        }

        return sl::DLSSMode::eMaxQuality;
    }

    // Row by row: four floats each, in the order streamlineMatrix wrote them.
    void copyMatrix(sl::float4x4& target, const std::array<float, 16>& values) {
        for (std::uint32_t row = 0; row < 4; ++row) {
            target.setRow(row, sl::float4{values[row * 4], values[row * 4 + 1], values[row * 4 + 2], values[row * 4 + 3]});
        }
    }

    sl::float3 slVector(const Vec3& vector) {
        return sl::float3{vector.x, vector.y, vector.z};
    }

    // An image as Streamline names it: its handle, view, the layout it is in when DLSS reads or writes it, and the
    // facts Vulkan cannot be asked for afterwards (size, format, uses).
    sl::Resource slResource(const VulkanImage& image, VkImageLayout layout) {
        const ImageDescription& description = image.getDescription();
        sl::Resource resource{sl::ResourceType::eTex2d, reinterpret_cast<void*>(image.get()), nullptr, reinterpret_cast<void*>(image.getView()),
                              static_cast<std::uint32_t>(layout)};
        resource.width = description.width;
        resource.height = description.height;
        resource.nativeFormat = static_cast<std::uint32_t>(description.format);
        resource.mipLevels = description.levels;
        resource.arrayLayers = 1;
        resource.flags = 0;
        resource.usage = description.usage;
        return resource;
    }
}

/*
* Streamline is told: manual hooking (the engine makes Vulkan and calls it itself), Vulkan, DLSS as the one feature,
* plugins from beside the exe, images tagged per frame (each frame's token with its tags), no over-the-air updates
* (NVIDIA's own update of DLSS) and no command list state tracking (it needs the hooks manual hooking leaves out). Then DLSS's requirements are kept for the device, all but the
* EXT form of buffer device addresses: Vulkan forbids it beside the KHR one, which is core in 1.2 anyway.
*/
StreamlineLibrary::StreamlineLibrary() {
    SDL_SharedObject* library = interposerLibrary();

    if (library == nullptr) {
        return;
    }

    functions = std::make_unique<StreamlineFunctions>();
    StreamlineFunctions& loaded = *functions;
    loaded.init = loadFunction<PFun_slInit>(library, "slInit");
    loaded.shutdown = loadFunction<PFun_slShutdown>(library, "slShutdown");
    loaded.featureRequirements = loadFunction<PFun_slGetFeatureRequirements>(library, "slGetFeatureRequirements");
    loaded.featureSupported = loadFunction<PFun_slIsFeatureSupported>(library, "slIsFeatureSupported");
    loaded.setVulkanInfo = loadFunction<PFun_slSetVulkanInfo>(library, "slSetVulkanInfo");
    loaded.featureFunction = loadFunction<PFun_slGetFeatureFunction>(library, "slGetFeatureFunction");
    loaded.newFrameToken = loadFunction<PFun_slGetNewFrameToken>(library, "slGetNewFrameToken");
    loaded.setConstants = loadFunction<PFun_slSetConstants>(library, "slSetConstants");
    loaded.setTags = loadFunction<PFun_slSetTagForFrame>(library, "slSetTagForFrame");
    loaded.evaluate = loadFunction<PFun_slEvaluateFeature>(library, "slEvaluateFeature");
    loaded.freeResources = loadFunction<PFun_slFreeResources>(library, "slFreeResources");
    loaded.getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(SDL_LoadFunction(library, "vkGetDeviceProcAddr"));

    if (!loaded.init || !loaded.shutdown || !loaded.featureRequirements || !loaded.featureSupported || !loaded.setVulkanInfo ||
        !loaded.featureFunction || !loaded.newFrameToken || !loaded.setConstants || !loaded.setTags || !loaded.evaluate || !loaded.freeResources ||
        !loaded.getDeviceProcAddr) {
        SDL_Log("DLSS unavailable: sl.interposer.dll lacks Streamline's functions");
        functions.reset();
        return;
    }

    const std::wstring folder = executableFolder();
    const wchar_t* pluginPaths[] = {folder.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS};

    sl::Preferences preferences{};
    preferences.logLevel = sl::LogLevel::eDefault;
    preferences.logMessageCallback = logStreamlineMessage;
    preferences.pathsToPlugins = pluginPaths;
    preferences.numPathsToPlugins = 1;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                        sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 1;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "1";
    preferences.projectId = projectId;
    preferences.renderAPI = sl::RenderAPI::eVulkan;

    if (loaded.init(preferences, sl::kSDKVersion) != sl::Result::eOk) {
        SDL_Log("DLSS unavailable: Streamline did not start");
        functions.reset();
        return;
    }

    initialised = true;
    sl::FeatureRequirements requirements{};

    if (loaded.featureRequirements(sl::kFeatureDLSS, requirements) != sl::Result::eOk) {
        SDL_Log("DLSS unavailable: Streamline did not load it");
        shutdown();
        return;
    }

    for (std::uint32_t index = 0; index < requirements.vkNumInstanceExtensions; ++index) {
        extensions.instance.emplace_back(requirements.vkInstanceExtensions[index]);
    }

    for (std::uint32_t index = 0; index < requirements.vkNumDeviceExtensions; ++index) {
        if (std::strcmp(requirements.vkDeviceExtensions[index], VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) != 0) {
            extensions.device.emplace_back(requirements.vkDeviceExtensions[index]);
        }
    }
}

StreamlineLibrary::~StreamlineLibrary() {
    shutdown();
}

bool StreamlineLibrary::isLoaded() const {
    return initialised && functions != nullptr;
}

const OptionalExtensions& StreamlineLibrary::getExtensions() const {
    return extensions;
}

void StreamlineLibrary::shutdown() {
    if (initialised && functions != nullptr) {
        functions->shutdown();
    }

    initialised = false;
    functions.reset();
    extensions = OptionalExtensions{};
}

const StreamlineFunctions* StreamlineLibrary::getFunctions() const {
    return isLoaded() ? functions.get() : nullptr;
}

PFN_vkQueuePresentKHR StreamlineLibrary::presentFunction(VkDevice device) const {
    if (!isLoaded()) {
        return nullptr;
    }

    return reinterpret_cast<PFN_vkQueuePresentKHR>(functions->getDeviceProcAddr(device, "vkQueuePresentKHR"));
}

/*
* Streamline gets the device, instance and GPU, and where its own queues would start after the engine's one (DLSS
* super resolution asks for none). Then whether DLSS runs on this GPU, and its own functions.
*/
VulkanDlss::VulkanDlss(const VulkanDevice& device, const StreamlineLibrary& library):
    device(device),
    functions(library.getFunctions())
{
    if (functions == nullptr) {
        return;
    }

    if (!device.hasOptionalExtensions()) {
        SDL_Log("DLSS unavailable: the GPU lacks the Vulkan extensions it needs");
        functions = nullptr;
        return;
    }

    sl::VulkanInfo vulkan{};
    vulkan.device = device.getDevice();
    vulkan.instance = device.getInstance();
    vulkan.physicalDevice = device.getPhysicalDevice();
    vulkan.graphicsQueueFamily = device.getQueueFamily();
    vulkan.graphicsQueueIndex = 1;
    vulkan.computeQueueFamily = device.getQueueFamily();
    vulkan.computeQueueIndex = 1;

    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = device.getPhysicalDevice();

    if (functions->setVulkanInfo(vulkan) != sl::Result::eOk || functions->featureSupported(sl::kFeatureDLSS, adapter) != sl::Result::eOk) {
        SDL_Log("DLSS unavailable: this GPU does not run it");
        functions = nullptr;
        return;
    }

    available = true;
}

bool VulkanDlss::isAvailable() const {
    return available;
}

/*
* DLSS's optimal settings for the mode give the range of render sizes it takes (Quality at 2560 x 1440: 1280 x 720 to
* 2560 x 1440; Ultra performance only 853 x 480), asked once per output size and mode.
*/
RenderSize VulkanDlss::renderSize(std::uint32_t outputWidth, std::uint32_t outputHeight, float scale) {
    const RenderSize wanted = ::renderSize(outputWidth, outputHeight, scale);

    if (!available) {
        return wanted;
    }

    const UpscalingMode mode = nearestUpscalingMode(scale);
    const auto key = std::make_pair((std::uint64_t{outputWidth} << 32) | outputHeight, mode);
    auto found = ranges.find(key);

    if (found == ranges.end()) {
        void* function = nullptr;
        functions->featureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", function);
        sl::DLSSOptions options{};
        options.mode = dlssMode(mode);
        options.outputWidth = outputWidth;
        options.outputHeight = outputHeight;
        sl::DLSSOptimalSettings optimal{};

        if (function == nullptr || reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(function)(options, optimal) != sl::Result::eOk) {
            return wanted;
        }

        found = ranges.emplace(key, std::make_pair(RenderSize{optimal.renderWidthMin, optimal.renderHeightMin},
                                                   RenderSize{optimal.renderWidthMax, optimal.renderHeightMax})).first;
    }

    return clampRenderSize(wanted, found->second.first, found->second.second);
}

void VulkanDlss::ensureSize(std::uint32_t outputWidth, std::uint32_t outputHeight) {
    if (width == outputWidth && height == outputHeight) {
        return;
    }

    if (available && width != 0) {
        functions->freeResources(sl::kFeatureDLSS, sl::ViewportHandle{viewportId});
    }

    ImageDescription description;
    description.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    description.width = outputWidth;
    description.height = outputHeight;
    // Storage: DLSS writes it; sampled: eye adaptation, bloom and tone mapping read it; transfer source: captures copy
    // it; transfer destination: DLSS clears it when it starts afresh.
    description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    description.name = "DLSS output";
    output = VulkanImage{device, description};
    width = outputWidth;
    height = outputHeight;
}

/*
* The order Streamline wants: a frame token, then the frame's constants, the options and the tags, all for the same
* token and viewport, then the evaluation. The images are put in the layouts they are tagged with first (colour, depth
* and velocity SHADER_READ_ONLY_OPTIMAL, the output GENERAL); DLSS records its own barriers and returns them there.
* Its matrices take no jitter (it is passed on its own), its motions are this frame's to last frame's in shares of the
* screen (the inputs' scale), its depth is reversed, and the camera's motion is already in the velocity image.
*/
bool VulkanDlss::dispatch(VkCommandBuffer commands, VulkanImage& colour, VulkanImage& depth, VulkanImage& velocity, const DlssFrameInputs& inputs,
                          UpscalingMode mode, std::uint32_t frameIndex) {
    if (!available) {
        return false;
    }

    const sl::ViewportHandle viewport{viewportId};
    sl::FrameToken* frame = nullptr;

    if (functions->newFrameToken(frame, &frameIndex) != sl::Result::eOk || frame == nullptr) {
        SDL_Log("DLSS unavailable: Streamline gave no frame token");
        available = false;
        return false;
    }

    sl::Constants constants{};
    copyMatrix(constants.cameraViewToClip, inputs.viewToClip);
    copyMatrix(constants.clipToCameraView, inputs.clipToView);
    copyMatrix(constants.clipToPrevClip, inputs.clipToPrevClip);
    copyMatrix(constants.prevClipToClip, inputs.prevClipToClip);
    copyMatrix(constants.clipToLensClip, streamlineMatrix(Mat4::identity()));
    constants.jitterOffset = sl::float2{inputs.jitter.x, inputs.jitter.y};
    constants.mvecScale = sl::float2{inputs.motionVectorScale.x, inputs.motionVectorScale.y};
    constants.cameraPinholeOffset = sl::float2{0.0f, 0.0f};
    constants.cameraPos = slVector(inputs.camera.position);
    constants.cameraUp = slVector(inputs.camera.up);
    constants.cameraRight = slVector(inputs.camera.right);
    constants.cameraFwd = slVector(inputs.camera.forward);
    constants.cameraNear = inputs.camera.cameraNear;
    constants.cameraFar = inputs.camera.cameraFar;
    constants.cameraFOV = inputs.camera.verticalFov;
    constants.cameraAspectRatio = inputs.camera.aspectRatio;
    constants.depthInverted = sl::Boolean::eTrue;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = inputs.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;

    void* setOptions = nullptr;
    functions->featureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", setOptions);

    sl::DLSSOptions options{};
    options.mode = dlssMode(mode);
    options.outputWidth = inputs.outputWidth;
    options.outputHeight = inputs.outputHeight;
    options.colorBuffersHDR = sl::Boolean::eTrue;
    options.useAutoExposure = sl::Boolean::eTrue;
    // The chosen model in every mode, so a custom scale's nearest mode runs it too; DLSS rebuilds itself on a change.
    const sl::DLSSPreset preset = static_cast<sl::DLSSPreset>(inputs.preset);
    options.dlaaPreset = preset;
    options.qualityPreset = preset;
    options.balancedPreset = preset;
    options.performancePreset = preset;
    options.ultraPerformancePreset = preset;
    options.ultraQualityPreset = preset;

    if (setOptions == nullptr || functions->setConstants(constants, *frame, viewport) != sl::Result::eOk ||
        reinterpret_cast<PFun_slDLSSSetOptions*>(setOptions)(viewport, options) != sl::Result::eOk) {
        SDL_Log("DLSS unavailable: Streamline refused the frame's constants or options");
        available = false;
        return false;
    }

    colour.transition(commands, ImageState::ComputeRead);
    depth.transition(commands, ImageState::ComputeRead);
    velocity.transition(commands, ImageState::ComputeRead);
    output.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    sl::Resource colourIn = slResource(colour, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sl::Resource depthIn = slResource(depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sl::Resource motion = slResource(velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sl::Resource colourOut = slResource(output, VK_IMAGE_LAYOUT_GENERAL);
    const sl::Extent renderExtent{0, 0, inputs.renderSize.width, inputs.renderSize.height};
    const sl::Extent outputExtent{0, 0, inputs.outputWidth, inputs.outputHeight};

    // Valid until the frame is presented, as all four are (the next frame writes them again): Streamline then reads
    // them where they are, where a shorter lifecycle would have it copy each first.
    const std::array<sl::ResourceTag, 4> tags{
        sl::ResourceTag{&colourIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent},
        sl::ResourceTag{&depthIn, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent},
        sl::ResourceTag{&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent},
        sl::ResourceTag{&colourOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &outputExtent},
    };

    sl::CommandBuffer* commandBuffer = reinterpret_cast<sl::CommandBuffer*>(commands);
    const sl::BaseStructure* evaluated[] = {&viewport};

    // Refused, nothing was recorded but the barriers above (to states the resolve takes from too), so the frame falls
    // back to temporal anti-aliasing and DLSS stays off.
    if (functions->setTags(*frame, viewport, tags.data(), static_cast<std::uint32_t>(tags.size()), commandBuffer) != sl::Result::eOk ||
        functions->evaluate(sl::kFeatureDLSS, *frame, evaluated, 1, commandBuffer) != sl::Result::eOk) {
        SDL_Log("DLSS unavailable: Streamline refused to evaluate it (see its messages above)");
        available = false;
        return false;
    }

    output.transition(commands, ImageState::SurfaceRead);
    return true;
}

VulkanImage& VulkanDlss::getOutput() {
    return output;
}
