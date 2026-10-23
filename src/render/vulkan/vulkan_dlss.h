#pragma once

#include "render/gpu/upscaling.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <map>
#include <memory>
#include <utility>

struct StreamlineFunctions;

/*
* NVIDIA's Streamline (external/streamline, fetched by tools/fetch_streamline.py, not committed: the NVIDIA RTX SDK
* licence), the library DLSS is reached through: sl.interposer.dll, its signature checked, then loaded at run time like
* AMD's FSR, since its functions are C ones (extern "C", plain structs), which GCC can call though NVIDIA builds it with
* Microsoft's compiler. It must start before the Vulkan device is made, since DLSS needs extensions on it: this asks
* Streamline for them (getExtensions), the renderer hands them to VulkanDevice, and VulkanDlss connects Streamline to
* the device afterwards. Manual hooking: the engine keeps making and calling Vulkan itself, and tells Streamline what
* it needs to know. Without the DLLs, or on a GPU without DLSS, everything here stays off and the game carries on.
*/
class StreamlineLibrary {
public:
    StreamlineLibrary();
    ~StreamlineLibrary();

    StreamlineLibrary(const StreamlineLibrary&) = delete;
    StreamlineLibrary& operator=(const StreamlineLibrary&) = delete;

    // Whether Streamline started, with DLSS loaded.
    bool isLoaded() const;

    // The instance and device extensions DLSS needs (none when Streamline did not start).
    const OptionalExtensions& getExtensions() const;

    // Streamline stops before the device it was told about goes (the renderer's destructor calls it first); again is
    // nothing.
    void shutdown();

    const StreamlineFunctions* getFunctions() const;

    /*
    * Streamline's vkQueuePresentKHR for this device, which the swapchain presents through while DLSS may run: manual
    * hooking still needs Streamline to see every present, to keep its books and free what finished frames used. Null
    * when Streamline did not start.
    */
    PFN_vkQueuePresentKHR presentFunction(VkDevice device) const;

private:
    std::unique_ptr<StreamlineFunctions> functions;
    OptionalExtensions extensions;
    bool initialised = false;
};

/*
* DLSS super resolution (render/gpu/upscaling.h): like FSR it takes this frame's jittered colour and depth and the
* velocity image (a motion in every pixel, after the camera motion pass) at the render size, and writes the output
* image at the output size, which is then the finished picture. It works in its named modes, each taking render sizes
* in a range round its own (renderSize keeps the render scale's inside it). Each frame gets a frame token, the camera
* constants, the options (the mode, the output's size, HDR colour, its own exposure, NVIDIA's model for the mode) and
* the images tagged with the layouts they are in, which DLSS returns them to; then it is evaluated into the commands.
*/
class VulkanDlss {
public:
    VulkanDlss(const VulkanDevice& device, const StreamlineLibrary& library);

    VulkanDlss(const VulkanDlss&) = delete;
    VulkanDlss& operator=(const VulkanDlss&) = delete;

    // Whether Streamline started, the device has DLSS's extensions, and DLSS runs on this GPU.
    bool isAvailable() const;

    // The render size for this output and scale: the scale's own, kept within the range the nearest mode takes.
    RenderSize renderSize(std::uint32_t outputWidth, std::uint32_t outputHeight, float scale);

    // The output image at the output's size, and DLSS's own images freed when that changes. The GPU must be idle.
    void ensureSize(std::uint32_t outputWidth, std::uint32_t outputHeight);

    /*
    * Record DLSS: colour, depth, velocity and the reactive mask readable by compute, the output written as a storage image and left
    * readable by fragment and compute shaders. Returns false (and DLSS becomes unavailable) when Streamline refuses
    * the frame; nothing but barriers is recorded then.
    */
    bool dispatch(VkCommandBuffer commands, VulkanImage& colour, VulkanImage& depth, VulkanImage& velocity, VulkanImage& reactive,
                  const DlssFrameInputs& inputs, UpscalingMode mode, std::uint32_t frameIndex);

    // The last dispatch's result: the finished picture at the output's size.
    VulkanImage& getOutput();

private:
    const VulkanDevice& device;
    const StreamlineFunctions* functions = nullptr;
    bool available = false;
    VulkanImage output;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    // Each mode's range of render sizes for an output size, asked of DLSS once.
    std::map<std::pair<std::uint64_t, UpscalingMode>, std::pair<RenderSize, RenderSize>> ranges;
};
