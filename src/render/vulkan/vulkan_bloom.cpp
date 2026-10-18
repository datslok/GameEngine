#include "render/vulkan/vulkan_bloom.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_device.h"

#include <SDL3/SDL.h>
#include <utility>

namespace {
    PipelineDescription bloomPipeline(const char* fragmentShader, VkFormat format, BlendMode blend) {
        PipelineDescription description;
        description.vertexShader = "assets/shaders/fullscreen_triangle.vert.spv";
        description.fragmentShader = fragmentShader;
        description.resources.fragmentSamplers = 1;
        description.resources.fragmentUniforms = 1;
        description.colourFormat = format;
        description.blend = blend;
        return description;
    }
}

VulkanBloom::VulkanBloom(const VulkanDevice& device, DescriptorLayouts& layouts, VkSampler sampler):
    device(device),
    sampler(sampler)
{
    const VkFormatFeatureFlags2 needed = VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT |
                                         VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

    if (device.supportsFormat(VK_FORMAT_B10G11R11_UFLOAT_PACK32, needed)) {
        levelFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    }
    else {
        SDL_Log("Bloom: 11/10-bit float images not supported, using 16-bit floats");
    }

    // Shrinking overwrites a level; growing adds onto it (its own halving is already there).
    downsample = std::make_unique<VulkanPipeline>(device, layouts,
                                                  bloomPipeline("assets/shaders/bloom_downsample.frag.spv", levelFormat, BlendMode::None));
    upsample = std::make_unique<VulkanPipeline>(device, layouts,
                                                bloomPipeline("assets/shaders/bloom_upsample.frag.spv", levelFormat, BlendMode::Additive));
}

/*
* The levels follow the frame's size and the level count. The GPU may still be reading the old ones, so it is waited
* for before they go; this only happens on a resize or a settings change.
*/
void VulkanBloom::ensureLevels(std::uint32_t frameWidth, std::uint32_t frameHeight, std::uint32_t levelCount) {
    if (levels.size() == levelCount && levelsFrameWidth == frameWidth && levelsFrameHeight == frameHeight) {
        return;
    }

    std::vector<VulkanImage> replacement;
    replacement.reserve(levelCount);

    for (std::uint32_t level = 0; level < levelCount; ++level) {
        const BloomLevelSize size = bloomLevelSize(frameWidth, frameHeight, level);

        ImageDescription description;
        description.format = levelFormat;
        description.width = size.width;
        description.height = size.height;
        description.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        replacement.emplace_back(device, description);
    }

    device.waitIdle();
    levels = std::move(replacement);
    levelsFrameWidth = frameWidth;
    levelsFrameHeight = frameHeight;
}

/*
* One step: the target becomes a colour target (its old contents dropped when it is overwritten, kept when added onto),
* the triangle runs the filter over it reading the source, and the target becomes readable for the next step.
*/
void VulkanBloom::runPass(FrameContext& frame, const VulkanPipeline& pipeline, VulkanImage& target, bool keepTarget,
                          const VulkanImage& source, const BloomPassUniformData& uniforms) {
    const VkCommandBuffer commands = frame.commands;
    target.transition(commands, ImageState::ColourTarget, keepTarget ? ImageContents::Keep : ImageContents::Discard);

    const ColourAttachment colour{target.getView(), keepTarget ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE, {}};
    beginRendering(commands, {target.getDescription().width, target.getDescription().height}, &colour, nullptr);

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.get());
    frame.uniforms.push(VK_SHADER_STAGE_FRAGMENT_BIT, 0, &uniforms, sizeof(uniforms));
    frame.uniforms.bind(commands, pipeline);

    const SampledImage image{source.getView(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    bindFragmentResources(commands, frame.descriptors, pipeline, {&image, 1});
    drawFullScreenTriangle(commands);

    vkCmdEndRendering(commands);
    target.transition(commands, ImageState::FragmentRead);
}

// Down the chain (the Karis average on the first halving only), then back up from the second smallest level.
BloomComposite VulkanBloom::draw(FrameContext& frame, const VulkanImage& scene, std::uint32_t frameWidth, std::uint32_t frameHeight,
                                 const BloomSettings& settings) {
    drewLastFrame = false;

    const std::uint32_t levelCount = settings.enabled ? bloomLevelCount(frameWidth, frameHeight, settings.maxLevels) : 0;
    const BloomComposite composite = bloomComposite(settings, levelCount);

    if (composite.strength == 0.0f) {
        return composite;
    }

    ensureLevels(frameWidth, frameHeight, levelCount);

    const VulkanImage* source = &scene;
    BloomLevelSize sourceSize{frameWidth, frameHeight};

    for (std::uint32_t level = 0; level < levelCount; ++level) {
        const BloomLevelSize size = bloomLevelSize(frameWidth, frameHeight, level);
        const float karis = level == 0 && settings.fireflyReduction ? 1.0f : 0.0f;

        runPass(frame, *downsample, levels[level], false, *source, packBloomPass(sourceSize, size, karis));
        source = &levels[level];
        sourceSize = size;
    }

    for (std::uint32_t level = levelCount - 1; level > 0; --level) {
        const BloomLevelSize smaller = bloomLevelSize(frameWidth, frameHeight, level);
        const BloomLevelSize target = bloomLevelSize(frameWidth, frameHeight, level - 1);

        runPass(frame, *upsample, levels[level - 1], true, levels[level], packBloomPass(smaller, target, settings.radius));
    }

    drewLastFrame = true;
    return composite;
}

const VulkanImage* VulkanBloom::getResult() const {
    return drewLastFrame ? &levels.front() : nullptr;
}
