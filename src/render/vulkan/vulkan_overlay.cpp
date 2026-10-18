#include "render/vulkan/vulkan_overlay.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_device.h"

#include "imgui/imgui.h"
#include "imgui/imgui_impl_vulkan.h"

namespace {
    // Mirrors the OverlayData block in overlay.frag (std140, one vec4).
    struct OverlayUniformData {
        float display[4]; // x: what SDR white is written as
    };

    static_assert(sizeof(OverlayUniformData) == 16, "OverlayUniformData must match the shader's OverlayData block");
}

VulkanOverlay::VulkanOverlay(const VulkanDevice& device, DescriptorLayouts& layouts, VkSampler sampler):
    device(device),
    layouts(layouts),
    sampler(sampler)
{
}

// Premultiplied blending: what is already there shows through by how little the overlay covers it.
void VulkanOverlay::setTargetFormat(VkFormat format) {
    if (format == targetFormat && pipeline) {
        return;
    }

    PipelineDescription description;
    description.vertexShader = "assets/shaders/fullscreen_triangle.vert.spv";
    description.fragmentShader = "assets/shaders/overlay.frag.spv";
    description.resources.fragmentSamplers = 1;
    description.resources.fragmentUniforms = 1;
    description.colourFormat = format;
    description.blend = BlendMode::PremultipliedAlpha;

    pipeline = std::make_unique<VulkanPipeline>(device, layouts, description);
    targetFormat = format;
}

// The new image is made before the old one goes; the GPU may still read the old one, so it is waited for.
void VulkanOverlay::ensureImage(std::uint32_t width, std::uint32_t height) {
    if (image && image.getDescription().width == width && image.getDescription().height == height) {
        return;
    }

    ImageDescription description;
    description.format = vulkanOverlayFormat;
    description.width = width;
    description.height = height;
    description.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    description.name = "Debug UI overlay";

    VulkanImage replacement{device, description};
    device.waitIdle();
    image = std::move(replacement);
}

/*
* ImGui's backend uploads a changed texture (the font atlas, when it grows) with its own command buffer and waits for
* it, which must not happen while this frame's pass is open, so changed textures are uploaded first. Then the UI is
* drawn into the overlay, cleared to transparent, and the overlay becomes readable for the composite.
*/
void VulkanOverlay::drawUi(FrameContext& frame, ImDrawData* drawData, std::uint32_t frameWidth, std::uint32_t frameHeight) {
    ensureImage(frameWidth, frameHeight);

    if (drawData->Textures != nullptr) {
        for (ImTextureData* texture : *drawData->Textures) {
            if (texture->Status != ImTextureStatus_OK) {
                ImGui_ImplVulkan_UpdateTexture(texture);
            }
        }
    }

    const VkCommandBuffer commands = frame.commands;
    image.transition(commands, ImageState::ColourTarget, ImageContents::Discard);

    const ColourAttachment colour{image.getView(), VK_ATTACHMENT_LOAD_OP_CLEAR, {0.0f, 0.0f, 0.0f, 0.0f}};
    beginRendering(commands, {frameWidth, frameHeight}, &colour, nullptr);
    ImGui_ImplVulkan_RenderDrawData(drawData, commands);
    vkCmdEndRendering(commands);

    image.transition(commands, ImageState::FragmentRead);
}

void VulkanOverlay::composite(FrameContext& frame, float whiteLevel) {
    if (!image) {
        return;
    }

    const VkCommandBuffer commands = frame.commands;
    const OverlayUniformData uniforms{{whiteLevel, 0.0f, 0.0f, 0.0f}};

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->get());
    frame.uniforms.push(VK_SHADER_STAGE_FRAGMENT_BIT, 0, &uniforms, sizeof(uniforms));
    frame.uniforms.bind(commands, *pipeline);

    const SampledImage overlay{image.getView(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    bindFragmentResources(commands, frame.descriptors, *pipeline, {&overlay, 1});
    drawFullScreenTriangle(commands);
}
