#pragma once

#include "math/vec3.h"
#include "render/gpu/reflection_probe.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <vector>

class VulkanDevice;

/*
* The reflection probe's cubes on the GPU (render/gpu/reflection_probe.h): the capture (six faces of light, with mips,
* and their depths) the renderer draws into, then the distances and the prefiltered light the reflection pass reads.
* One probe, recaptured every frame where the camera is.
*/
class VulkanReflectionProbe {
public:
    // nearest: exact texels, clamped; trilinear: blended between texels and levels, clamped, every level.
    VulkanReflectionProbe(const VulkanDevice& device, VkSampler nearest, VkSampler trilinear);

    // The cubes for faces this many texels a side, remade (and the capture forgotten) when it changes. The GPU must not
    // be using the old ones: the renderer waits for it first.
    void ensureSize(std::uint32_t size);
    std::uint32_t getSize() const;

    // The capture's faces made ready to be drawn into, their old contents discarded.
    void beginCapture(VkCommandBuffer commands);

    // A face's targets: its light, cleared to the background, and its depth, cleared to the far depth.
    ColourAttachment faceColour(int face, const Vec3& background) const;
    DepthAttachment faceDepth(int face) const;

    /*
    * After the six faces are drawn: the capture's mips, the distances (from the depths, for a capture from nearPlane to
    * farPlane) and the prefiltered levels, both left readable by compute. origin is where it was taken, in the world.
    */
    void finishCapture(VkCommandBuffer commands, FrameDescriptors& descriptors, float nearPlane, float farPlane, const Vec3& origin);

    // Whether a capture is ready to read, where it was taken, its prefiltered levels and its distance pyramid's levels.
    bool hasCapture() const;
    const Vec3& getOrigin() const;
    std::uint32_t getLevels() const;
    std::uint32_t getDistanceLevels() const;

    // The prefiltered light and the distances, as the reflection pass binds them: made readable first if nothing was
    // ever captured into them (they are bound even while unused).
    VkImageView readableLight(VkCommandBuffer commands);
    VkImageView readableDistances(VkCommandBuffer commands);

    // The prefiltered light as the glass pass reads it, in fragment shaders, with the trilinear sampler: made readable
    // there (as readableLight, defined even if never captured).
    SampledImage fragmentReadableLight(VkCommandBuffer commands);

private:
    const VulkanDevice& device;
    VkSampler nearest;
    VkSampler trilinear;
    std::unique_ptr<ComputePipeline> distancePipeline;
    std::unique_ptr<ComputePipeline> distanceMinPipeline;
    std::unique_ptr<ComputePipeline> prefilterPipeline;

    std::uint32_t size = 0;
    VulkanImage capture;    // the faces' light, with mips (filtered importance sampling reads them)
    VulkanImage depth;      // the faces' depths
    VulkanImage distances;  // how far the probe saw, per texel, and above it the nearest over each block (a pyramid)
    VulkanImage light;      // the prefiltered light, a level per roughness

    std::vector<OwnedHandle<VkImageView>> captureFaces; // each face's top level, a colour target
    std::vector<OwnedHandle<VkImageView>> depthFaces;   // each face's depth target
    std::vector<OwnedHandle<VkImageView>> distanceLevels; // each distance level's six faces as an array, written by compute
    std::vector<OwnedHandle<VkImageView>> lightLevels;  // each prefiltered level's six faces as an array

    bool captured = false;
    Vec3 origin;

    OwnedHandle<VkImageView> makeView(const VulkanImage& image, VkImageViewType type, std::uint32_t level, std::uint32_t firstLayer,
                                      std::uint32_t layers) const;
};
