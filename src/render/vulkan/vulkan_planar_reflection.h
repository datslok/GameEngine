#pragma once

#include "render/gpu/upscaling.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_colour_pyramid.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

class FrameDescriptors;
class VulkanDevice;

/*
* The mirror's picture on the GPU (render/gpu/planar_reflection.h): the colour and depth targets the renderer draws the
* mirrored scene into, then its ever blurrier levels for rough mirrors, which the reflection pass reads.
*/
class VulkanPlanarReflection {
public:
    // linear: bilinear and clamped, for the blurrier levels.
    VulkanPlanarReflection(const VulkanDevice& device, VkSampler linear, VkFormat colourFormat, VkFormat depthFormat);

    // The images at this size, remade when it changes; the GPU must be idle (the renderer waits).
    void ensureSize(RenderSize render);
    RenderSize getSize() const;

    // The targets made ready to be drawn into, their old contents discarded, and how to draw into them: cleared to the
    // background, and to the far depth.
    void beginDraw(VkCommandBuffer commands);
    ColourAttachment colourTarget(const Vec3& background) const;
    DepthAttachment depthTarget() const;

    // After the scene is drawn: the blurrier levels, left readable by compute.
    void finishDraw(VkCommandBuffer commands, FrameDescriptors& descriptors);

    // The picture and its levels, as the reflection pass binds them.
    VulkanImage& getPicture();
    std::uint32_t getLevels() const;

    // Its depths, kept after drawing, for measuring how far the mirror shows (the mirror's sun cascades are fitted to
    // them, render/gpu/shadow_map.h, MirrorShadowView).
    VulkanImage& getDepth();

private:
    const VulkanDevice& device;
    VkFormat colourFormat;
    VkFormat depthFormat;
    VulkanColourPyramid pyramid;

    RenderSize size;
    VulkanImage colour;
    VulkanImage depth;
};
