#include "render/vulkan/render_pass.h"

#include <stdexcept>
#include <vector>

void beginRendering(VkCommandBuffer commands, VkExtent2D extent, const ColourAttachment* colour, const DepthAttachment* depth) {
    beginRendering(commands, extent, colour != nullptr ? std::span<const ColourAttachment>{colour, 1} : std::span<const ColourAttachment>{}, depth);
}

void beginRendering(VkCommandBuffer commands, VkExtent2D extent, std::span<const ColourAttachment> colours, const DepthAttachment* depth) {
    std::vector<VkRenderingAttachmentInfo> colourInfos(colours.size());
    VkRenderingAttachmentInfo depthInfo{};

    for (std::size_t index = 0; index < colours.size(); ++index) {
        const ColourAttachment& colour = colours[index];
        VkRenderingAttachmentInfo& colourInfo = colourInfos[index];
        colourInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colourInfo.imageView = colour.view;
        colourInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colourInfo.loadOp = colour.load;
        colourInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colourInfo.clearValue.color = {{colour.clear[0], colour.clear[1], colour.clear[2], colour.clear[3]}};
    }

    if (depth != nullptr) {
        depthInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthInfo.imageView = depth->view;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthInfo.loadOp = depth->load;
        depthInfo.storeOp = depth->store;
        depthInfo.clearValue.depthStencil = {depth->clear, 0};
    }

    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea.extent = extent;
    info.layerCount = 1;
    info.colorAttachmentCount = static_cast<std::uint32_t>(colourInfos.size());
    info.pColorAttachments = colourInfos.empty() ? nullptr : colourInfos.data();
    info.pDepthAttachment = depth != nullptr ? &depthInfo : nullptr;
    vkCmdBeginRendering(commands, &info);

    setViewport(commands, 0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height));
    setScissor(commands, extent.width, extent.height);
}

void bindFragmentResources(VkCommandBuffer commands, FrameDescriptors& descriptors, const VulkanPipeline& pipeline,
                           std::span<const SampledImage> images, std::span<const VkDescriptorBufferInfo> storageBuffers) {
    const ShaderResources& resources = pipeline.getResources();

    if (images.size() != resources.fragmentSamplers || storageBuffers.size() != resources.fragmentStorageBuffers) {
        throw std::logic_error("A draw must give exactly the textures and storage buffers its shader reads");
    }

    if (images.empty() && storageBuffers.empty()) {
        return;
    }

    const VkDescriptorSet set = descriptors.allocate(pipeline.getFragmentResourceLayout());

    std::vector<VkDescriptorImageInfo> imageInfos(images.size());
    std::vector<VkWriteDescriptorSet> writes(images.size() + storageBuffers.size());

    for (std::size_t index = 0; index < images.size(); ++index) {
        imageInfos[index] = {images[index].sampler, images[index].view, images[index].layout};

        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = set;
        writes[index].dstBinding = static_cast<std::uint32_t>(index);
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[index].pImageInfo = &imageInfos[index];
    }

    for (std::size_t index = 0; index < storageBuffers.size(); ++index) {
        VkWriteDescriptorSet& write = writes[images.size() + index];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = static_cast<std::uint32_t>(images.size() + index);
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &storageBuffers[index];
    }

    vkUpdateDescriptorSets(descriptors.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.getLayout(), fragmentResourceSet, 1, &set, 0, nullptr);
}

void drawFullScreenTriangle(VkCommandBuffer commands) {
    vkCmdDraw(commands, 3, 1, 0, 0);
}
