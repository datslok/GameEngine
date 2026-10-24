#pragma once

#include "math/mat4.h"
#include "math/vec3.h"
#include "render/gpu/volumetric_fog.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <array>
#include <cstdint>
#include <memory>

class ImmediateCommands;
class VulkanDevice;

/*
* Volumetric fog on the GPU (render/gpu/volumetric_fog.h): the grid's images and its three passes. Two lighting grids
* take turns (this frame's written while last frame's is read as its history), and the integrated grid is what the
* picture pass and glass read (scene set 1, binding 28; a one-texel clear stand-in on frames without fog). Every pass
* reads the frame's scene sets (0 and 1: the fog record, the lights, the shadows, the clusters, the probes) before a set
* of its own.
*/
class VulkanFog {
public:
    // linear filters the history and the integrated grid; nearest reads exact texels (the lighting grid, the depth).
    VulkanFog(const VulkanDevice& device, ImmediateCommands& uploads, VkSampler linear, VkSampler nearest, VkDescriptorSetLayout texturesLayout,
              VkDescriptorSetLayout frameLayout);

    /*
    * The frame's fog record, and the grid readied for it: remade (after waiting for the GPU) when its size changes, last
    * frame's grid carried along only when it was drawn last frame at the same size and range. Without fog (none in the
    * frame, or the setting off) the record is off and the history forgotten.
    */
    GpuFog prepare(const Fog* fog, const FogSettings& settings, const Mat4& viewProjection, const Vec3& cameraOrigin, std::uint64_t frame,
                   float cameraFar);

    // Whether the frame prepare() readied draws fog.
    bool drawsFog() const;

    // The integrated grid as the scene set samples it: the real one on frames with fog, else the clear stand-in.
    SampledImage sampledVolume() const;

    /*
    * Record the lighting and integration passes (the scene's sets bound by the caller's set numbers), after the shadows
    * and the clusters' lists, which they read; the integrated grid left readable by fragment and compute shaders.
    */
    void light(VkCommandBuffer commands, FrameDescriptors& descriptors, VkDescriptorSet textures, VkDescriptorSet frameSet);

    // Record the picture pass: the scene image changed in place, the depth (left readable by compute) read.
    void apply(VkCommandBuffer commands, FrameDescriptors& descriptors, VkDescriptorSet textures, VkDescriptorSet frameSet, VulkanImage& picture,
               VulkanImage& depth, const GpuFogPicturePass& pass);

private:
    const VulkanDevice& device;
    VkSampler linear;
    VkSampler nearest;
    std::unique_ptr<ComputePipeline> lightPipeline;
    std::unique_ptr<ComputePipeline> integratePipeline;
    std::unique_ptr<ComputePipeline> applyPipeline;

    std::array<VulkanImage, 2> lighting;
    VulkanImage integrated;
    VulkanImage clear;
    int current = 0;
    int width = 0;
    int height = 0;
    int slices = 0;

    // Last frame's grid camera (without the jitter) and where the camera was, its range, and whether it drew fog.
    Mat4 previousViewProjection;
    Vec3 previousOrigin;
    float previousRange = 0.0f;
    bool previousKnown = false;
    bool fogThisFrame = false;

    void ensureGrid(const FogSettings& settings);
    void bindScene(VkCommandBuffer commands, const ComputePipeline& pipeline, VkDescriptorSet textures, VkDescriptorSet frameSet) const;
};
