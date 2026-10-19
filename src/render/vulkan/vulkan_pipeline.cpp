#include "render/vulkan/vulkan_pipeline.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <array>
#include <stdexcept>

namespace {
    enum LayoutKind {
        EmptyLayout,
        UniformLayout,
        FragmentResourceLayout
    };
}

DescriptorLayouts::DescriptorLayouts(VkDevice device):
    device(device)
{
}

VkDescriptorSetLayout DescriptorLayouts::find(int kind, std::uint32_t first, std::uint32_t second,
                                              const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    const auto key = std::make_tuple(kind, first, second);
    const auto found = layouts.find(key);

    if (found != layouts.end()) {
        return found->second.get();
    }

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout), "Descriptor set layout creation failed");
    layouts.emplace(key, OwnedHandle<VkDescriptorSetLayout>{device, layout, vkDestroyDescriptorSetLayout});
    return layout;
}

VkDescriptorSetLayout DescriptorLayouts::empty() {
    return find(EmptyLayout, 0, 0, {});
}

VkDescriptorSetLayout DescriptorLayouts::uniforms(VkShaderStageFlags stage, std::uint32_t count) {
    if (count == 0) {
        return empty();
    }

    if (count > maxStageUniforms) {
        throw std::invalid_argument("Too many uniform blocks for one shader stage");
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings(count);

    for (std::uint32_t binding = 0; binding < count; ++binding) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = stage;
    }

    return find(UniformLayout, stage, count, bindings);
}

VkDescriptorSetLayout DescriptorLayouts::fragmentResources(std::uint32_t samplers, std::uint32_t storageBuffers) {
    if (samplers == 0 && storageBuffers == 0) {
        return empty();
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings(samplers + storageBuffers);

    for (std::uint32_t binding = 0; binding < samplers + storageBuffers; ++binding) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = binding < samplers ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    return find(FragmentResourceLayout, samplers, storageBuffers, bindings);
}

/*
* The fixed-function state copies SDL_GPU's choices for the same pipeline descriptions, so the port draws the same
* pixels. In particular depth clamping is on (SDL's enable_depth_clip defaults to false): a depth past the near or far
* plane is clamped to it rather than the triangle being cut there, which lets shadow casters behind a light's near plane
* still write the nearest depth ("pancaking").
*/
VulkanPipeline::VulkanPipeline(const VulkanDevice& device, DescriptorLayouts& layouts, const PipelineDescription& description):
    resources(description.resources)
{
    const VkDevice handle = device.getDevice();

    if (description.sharedLayout != VK_NULL_HANDLE) {
        layout = description.sharedLayout;
    }
    else {
        fragmentResourceLayout = layouts.fragmentResources(resources.fragmentSamplers, resources.fragmentStorageBuffers);
        const std::array<VkDescriptorSetLayout, 4> setLayouts{
            layouts.empty(),
            layouts.uniforms(VK_SHADER_STAGE_VERTEX_BIT, resources.vertexUniforms),
            fragmentResourceLayout,
            layouts.uniforms(VK_SHADER_STAGE_FRAGMENT_BIT, resources.fragmentUniforms),
        };

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
        layoutInfo.pSetLayouts = setLayouts.data();

        VkPipelineLayout createdLayout = VK_NULL_HANDLE;
        vulkanCheck(vkCreatePipelineLayout(handle, &layoutInfo, nullptr, &createdLayout), "Pipeline layout creation failed");
        ownedLayout = OwnedHandle<VkPipelineLayout>{handle, createdLayout, vkDestroyPipelineLayout};
        layout = createdLayout;
    }

    const OwnedHandle<VkShaderModule> vertexShader = device.loadShader(description.vertexShader);
    const OwnedHandle<VkShaderModule> fragmentShader = device.loadShader(description.fragmentShader);

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexShader.get();
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentShader.get();
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertexBinding{};
    vertexBinding.binding = 0;
    vertexBinding.stride = description.vertices.stride;
    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    const bool hasVertices = !description.vertices.attributes.empty();

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = hasVertices ? 1 : 0;
    vertexInput.pVertexBindingDescriptions = &vertexBinding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(description.vertices.attributes.size());
    vertexInput.pVertexAttributeDescriptions = description.vertices.attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = description.topology;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.depthClampEnable = VK_TRUE;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = description.cullMode;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisample.minSampleShading = 1.0f;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = description.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = description.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = description.depthCompare;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    if (description.blend != BlendMode::None) {
        const VkBlendFactor destination = description.blend == BlendMode::Additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = destination;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = destination;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    const bool hasColour = description.colourFormat != VK_FORMAT_UNDEFINED;

    VkPipelineColorBlendStateCreateInfo colourBlend{};
    colourBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colourBlend.attachmentCount = hasColour ? 1 : 0;
    colourBlend.pAttachments = &blend;

    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = hasColour ? 1 : 0;
    rendering.pColorAttachmentFormats = &description.colourFormat;
    rendering.depthAttachmentFormat = description.depthFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colourBlend;
    info.pDynamicState = &dynamic;
    info.layout = layout;

    VkPipeline created = VK_NULL_HANDLE;
    vulkanCheck(vkCreateGraphicsPipelines(handle, VK_NULL_HANDLE, 1, &info, nullptr, &created), description.fragmentShader);
    pipeline = OwnedHandle<VkPipeline>{handle, created, vkDestroyPipeline};
}

VkPipeline VulkanPipeline::get() const {
    return pipeline.get();
}

VkPipelineLayout VulkanPipeline::getLayout() const {
    return layout;
}

const ShaderResources& VulkanPipeline::getResources() const {
    return resources;
}

VkDescriptorSetLayout VulkanPipeline::getFragmentResourceLayout() const {
    return fragmentResourceLayout;
}

void setViewport(VkCommandBuffer commands, float x, float y, float width, float height) {
    VkViewport viewport{};
    viewport.x = x;
    viewport.y = y + height;
    viewport.width = width;
    viewport.height = -height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commands, 0, 1, &viewport);
}

void setScissor(VkCommandBuffer commands, std::uint32_t width, std::uint32_t height) {
    VkRect2D scissor{};
    scissor.extent = {width, height};
    vkCmdSetScissor(commands, 0, 1, &scissor);
}
