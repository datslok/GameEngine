#pragma once

#include "render/gpu/bloom.h"
#include "render/vulkan/frame_context.h"
#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_pipeline.h"

#include <cstdint>
#include <memory>
#include <vector>

class VulkanDevice;

/*
* The bloom chain (render/gpu/bloom.h) on the GPU: the lit scene halved level by level into
* small float images, then grown back, each level adding the one below, so level 0 ends up holding the blurred scene for
* tone mapping to mix in. Each step is one pass of the full-screen triangle; between steps, the level just drawn becomes
* the next step's texture (a barrier from colour target to fragment read).
*/
class VulkanBloom {
public:
    // sampler: linear filtering, clamped at the edges (the filters are built around bilinear reads).
    VulkanBloom(const VulkanDevice& device, DescriptorLayouts& layouts, VkSampler sampler);

    // The scene image must be readable (FragmentRead). Returns how tone mapping mixes the result in: strength 0, with
    // nothing drawn, when bloom is off or the frame is too small for a level.
    BloomComposite draw(FrameContext& frame, const VulkanImage& scene, std::uint32_t frameWidth, std::uint32_t frameHeight,
                        const BloomSettings& settings);

    // The grown level 0 from the last draw(), readable, or null when it drew nothing.
    const VulkanImage* getResult() const;

private:
    const VulkanDevice& device;
    VkSampler sampler;

    // 11- and 10-bit floats with no alpha, half the memory of 16-bit floats and plenty for a blur; 16-bit floats
    // where a GPU cannot draw into, blend onto and filter the smaller format.
    VkFormat levelFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    std::unique_ptr<VulkanPipeline> downsample;
    std::unique_ptr<VulkanPipeline> upsample;

    std::vector<VulkanImage> levels;
    std::uint32_t levelsFrameWidth = 0;
    std::uint32_t levelsFrameHeight = 0;
    bool drewLastFrame = false;

    void ensureLevels(std::uint32_t frameWidth, std::uint32_t frameHeight, std::uint32_t levelCount);
    void runPass(FrameContext& frame, const VulkanPipeline& pipeline, VulkanImage& target, bool keepTarget, const VulkanImage& source,
                 const BloomPassUniformData& uniforms);
};
