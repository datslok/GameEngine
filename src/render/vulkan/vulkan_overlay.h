#pragma once

#include "render/vulkan/frame_context.h"
#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_pipeline.h"

#include <cstdint>
#include <memory>

struct ImDrawData;
class VulkanDevice;

// The overlay image's format: 8 bits per channel, written as ImGui expects (sRGB values, blended as on an 8-bit screen).
inline constexpr VkFormat vulkanOverlayFormat = VK_FORMAT_R8G8B8A8_UNORM;

/*
* The debug UI drawn into its own transparent image, then laid over the finished screen image last of all, as
* OverlayRenderer does: on an HDR screen the UI's colours are decoded and scaled to the white level (overlay.frag)
* instead of being shown at scRGB's dim 80 nits. ImGui's Vulkan backend draws the UI; this owns the image and the
* composite pipeline (built for the screen's format).
*/
class VulkanOverlay {
public:
    // sampler: exact texels (the overlay is the screen's size, one texel per pixel).
    VulkanOverlay(const VulkanDevice& device, DescriptorLayouts& layouts, VkSampler sampler);

    void setTargetFormat(VkFormat format);

    // Upload ImGui's changed textures, then draw the UI into the overlay image, cleared first. Outside any pass.
    void drawUi(FrameContext& frame, ImDrawData* drawData, std::uint32_t frameWidth, std::uint32_t frameHeight);

    // Blend the overlay over the open screen pass. whiteLevel is what SDR white is written as on this screen.
    void composite(FrameContext& frame, float whiteLevel);

private:
    const VulkanDevice& device;
    DescriptorLayouts& layouts;
    VkSampler sampler;
    std::unique_ptr<VulkanPipeline> pipeline;
    VkFormat targetFormat = VK_FORMAT_UNDEFINED;
    VulkanImage image;

    void ensureImage(std::uint32_t width, std::uint32_t height);
};
