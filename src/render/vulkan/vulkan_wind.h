#pragma once

#include "math/vec3.h"
#include "render/gpu/wind.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <array>
#include <memory>

class ImmediateCommands;
class VulkanDevice;

/*
* The wind force field on the GPU (render/gpu/wind.h): two spring images taking turns (this frame's written from last
* frame's), each a 2D array of WIND_CELLS square, a layer per cascade and category, and the pass stepping them. The
* draws read both (scene set 1, bindings 38 and 39: this frame's and last frame's, for motion vectors) through a wrapping
* sampler. The record (binding 37) is the frame's, written with the scene's buffers.
*/
class VulkanWind {
public:
    // wrapping: a linear sampler that repeats, so a place reads its cell whichever texel of the window holds it.
    VulkanWind(const VulkanDevice& device, ImmediateCommands& uploads, VkSampler wrapping);

    /*
    * The frame's record: each cascade's window round the camera, last frame's windows carried along while the field ran
    * last frame too; time and the step from the frame's real time. Without wind (none in the frame, or the setting off)
    * the record is off and the springs are forgotten.
    */
    GpuWindField prepare(const Wind* wind, const WindSettings& settings, const Vec3& cameraOrigin, float frameSeconds);

    // Whether the frame prepare() readied steps the springs.
    bool runs() const;

    // Record the pass (after the record is written to record): this frame's springs written, left readable by vertex shaders.
    void step(VkCommandBuffer commands, FrameDescriptors& descriptors, VkBuffer record);

    // This frame's springs and last frame's, as the scene set samples them.
    SampledImage sampledSprings() const;
    SampledImage sampledPreviousSprings() const;

private:
    const VulkanDevice& device;
    VkSampler wrapping;
    std::unique_ptr<ComputePipeline> pipeline;
    std::array<VulkanImage, 2> springs;
    int current = 0;

    std::array<WindWindow, WIND_CASCADES> previousWindows{};
    bool previousKnown = false;
    bool runsThisFrame = false;
    float seconds = 0.0f;
};
