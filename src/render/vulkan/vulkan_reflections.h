#pragma once

#include "render/gpu/screen_reflections.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_colour_pyramid.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_reflection_probe.h"
#include "volk/volk.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

class VulkanDevice;

// The sets a ray-traced reflection pass reads before its own: every texture, the frame's scene and the ray tracing set.
struct ReflectionRaySets {
    VkDescriptorSet textures = VK_NULL_HANDLE;
    VkDescriptorSet scene = VK_NULL_HANDLE;
    VkDescriptorSet rayScene = VK_NULL_HANDLE;
};

/*
* Screen-space reflections on the GPU (render/gpu/screen_reflections.h): the reflection surfaces the main pass writes
* beside the scene image (normal, specular albedo and roughness, the stand-in), the depth pyramid built from this
* frame's depth, the colour pyramid built from last frame's finished picture, and the reflection pass that changes the
* scene image in place. All at the render size.
*/
class VulkanReflections {
public:
    // nearest: exact texels, clamped; linear: bilinear, clamped, one level; pyramid: trilinear, clamped, every level;
    // nearestLevels: exact texels, clamped, every level (the probe's distance pyramid).
    VulkanReflections(const VulkanDevice& device, VkSampler nearest, VkSampler linear, VkSampler pyramid, VkSampler nearestLevels,
                      std::uint32_t framesInFlight);

    // One pass record per frame in flight, since the CPU writes the next frame's while the GPU may read the last.
    void setFramesInFlight(std::uint32_t count);

    // On GPUs with ray queries: the pass that traces each mirror ray (reflections_rays.comp), reading the scene's sets
    // (these layouts: textures, the frame's scene, the ray tracing set) before its own.
    void enableRayTracing(VkDescriptorSetLayout textures, VkDescriptorSetLayout scene, VkDescriptorSetLayout rayScene);

    // The surfaces and pyramids at the render size, remade when it changes; the GPU must be idle (the renderer waits).
    void ensureSize(RenderSize render);

    // The surfaces' formats, in output location order (1, 2, 3), for the main pass's pipelines.
    std::vector<VkFormat> surfaceFormats() const;

    // The surfaces as the main pass's colour targets 1, 2 and 3, made ready to be drawn into (their old contents
    // discarded and not cleared: the reflection pass reads only pixels with a surface, which the main pass wrote).
    std::array<ColourAttachment, 3> beginSurfaces(VkCommandBuffer commands);

    /*
    * Record the pyramids and the reflection pass: the depth (drawn this frame), the velocity image (drawn this frame),
    * last frame's finished picture (any size; readable; null when screen-space reflections do not run), the reflection
    * probe (read only if the pass says so) and the scene image (drawn this frame), changed in place and left readable
    * by fragment and compute shaders, as the main pass leaves it. With rays (enableRayTracing first), each mirror ray is
    * traced through the scene in place of the probe's search.
    */
    void reflect(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& scene, VulkanImage& depth,
                 VulkanImage& velocity, VulkanImage* previousPicture, VulkanReflectionProbe& probe, const GpuReflectionPass& pass,
                 const ReflectionRaySets* rays = nullptr);

    bool tracesRays() const;

    // The depth pyramid's levels at the current size (packReflectionPass needs them).
    std::uint32_t getPyramidLevels() const;

private:
    const VulkanDevice& device;
    VkSampler nearest;
    VkSampler linear;
    VkSampler pyramidSampler;
    VkSampler nearestLevels;
    VkFormat standInFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    std::unique_ptr<ComputePipeline> depthPyramidPipeline;
    std::unique_ptr<ComputePipeline> reflectionPipeline;
    std::unique_ptr<ComputePipeline> rayReflectionPipeline; // null without ray queries
    std::vector<VulkanBuffer> passRecords;

    RenderSize size;
    VulkanImage normals;
    VulkanImage properties;
    VulkanImage standIns;
    VulkanImage depthPyramid;
    std::unique_ptr<VulkanColourPyramid> colourPyramid;

    // A view of each depth pyramid level alone, for writing one level at a time.
    std::vector<OwnedHandle<VkImageView>> depthLevels;

    void buildDepthPyramid(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& depth);
};
