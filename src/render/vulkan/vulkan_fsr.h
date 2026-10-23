#pragma once

#include "render/gpu/upscaling.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>

class VulkanDevice;
struct SDL_SharedObject;

/*
* AMD's FSR 3.1 upscaler (render/gpu/upscaling.h), through the FidelityFX API: five C functions in AMD's signed
* amd_fidelityfx_vk.dll (external/fidelityfx, v1.1.4, the SDK's last line with Vulkan), loaded at start-up by name, so
* nothing links against it and the game runs without it (FSR is then unavailable, and the engine's own temporal
* anti-aliasing upscales). A C interface is what lets a DLL built by Microsoft's compiler be called from GCC: C++'s
* names, classes and exceptions differ between compilers, C's functions and structs do not on 64-bit Windows.
*
* It takes this frame's jittered colour and depth and the velocity image (a motion in every pixel, after the camera
* motion pass) at the render size, and writes the output image at the output size, which is then the finished picture.
* Its context (FSR's own history and working images) is made the first time FSR runs and remade when the output's size
* changes; it records its own barriers, from and back to the states it is told the images are in.
*/
class VulkanFsr {
public:
    explicit VulkanFsr(const VulkanDevice& device);
    ~VulkanFsr();

    VulkanFsr(const VulkanFsr&) = delete;
    VulkanFsr& operator=(const VulkanFsr&) = delete;

    // Whether the DLL loaded and the GPU has the features its shaders use (and no context failed to be made).
    bool isAvailable() const;

    /*
    * The output image at the output's size, and the context dropped when that changes, to be made again at the next
    * dispatch. The GPU must be idle (the renderer waits before calling it on a resize, as for its other frame images).
    */
    void ensureSize(std::uint32_t outputWidth, std::uint32_t outputHeight);

    /*
    * Record FSR: colour and velocity readable, depth readable by compute, the output written as a storage image and left
    * readable by fragment and compute shaders. Returns false (and FSR becomes unavailable) when its context cannot be
    * made; nothing is recorded then.
    */
    bool dispatch(VkCommandBuffer commands, VulkanImage& colour, VulkanImage& depth, VulkanImage& velocity, const FsrFrameInputs& inputs);

    // The last dispatch's result: the finished picture at the output's size.
    VulkanImage& getOutput();

private:
    const VulkanDevice& device;
    SDL_SharedObject* library = nullptr;

    // The FidelityFX API's functions, as loaded from the DLL (its own PfnFfx* types, kept out of this header).
    void* createContext = nullptr;
    void* destroyContext = nullptr;
    void* dispatchFunction = nullptr;

    void* context = nullptr;
    bool available = false;
    VulkanImage output;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool makeContext();
    void dropContext();
};
