#include "render/vulkan/vulkan_texture.h"
#include "render/gpu/mip_levels.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

/*
* Image files store colour sRGB-encoded, and an _SRGB format makes the GPU decode texels to linear light when sampling
* (and when averaging mip levels), so lighting adds up real amounts of light. Data (roughness, metallic, normals) is
* read exactly as stored, so it uses the plain format.
*/
VulkanTextureDescription surfaceTexture(TextureEncoding encoding) {
    VulkanTextureDescription description;
    description.format = encoding == TextureEncoding::Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    return description;
}

/*
* The upload: level 0 is copied in from an upload buffer, then each smaller level is blitted from the one above. Every
* level gets its own barriers: it is written (transfer destination), then read for the next level (transfer source),
* then left for fragment shaders. The last level is never a source, so it goes straight to being read.
*/
VulkanTexture::VulkanTexture(const VulkanDevice& device, ImmediateCommands& uploads, std::uint32_t width, std::uint32_t height,
                             std::span<const std::uint8_t> texels, const VulkanTextureDescription& description):
    surface(description.surface)
{
    if (width == 0 || height == 0) {
        throw std::invalid_argument("Texture dimensions must be positive");
    }

    const std::uint64_t byteCount = std::uint64_t{width} * height * description.bytesPerTexel;

    if (description.bytesPerTexel == 0 || byteCount > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("Texture upload is too large");
    }

    if (texels.size() != byteCount) {
        throw std::invalid_argument("Texture data does not match its size and texel format");
    }

    const std::uint32_t levels = description.surface ? mipLevelCount(width, height) : 1;

    ImageDescription imageDescription;
    imageDescription.format = description.format;
    imageDescription.width = width;
    imageDescription.height = height;
    imageDescription.levels = levels;
    imageDescription.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (levels > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    image = VulkanImage{device, imageDescription};

    VulkanBuffer staging{device.getAllocator(), byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Upload};
    std::memcpy(staging.getMapped(), texels.data(), texels.size());
    staging.flush();

    uploads.run([&](VkCommandBuffer commands) {
        const VkImage handle = image.get();
        recordImageBarrier(commands, imageBarrier(handle, VK_IMAGE_ASPECT_COLOR_BIT, ImageState::Undefined,
                                                  ImageState::TransferDestination, ImageContents::Discard));

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(commands, staging.get(), handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        for (std::uint32_t level = 1; level < levels; ++level) {
            const std::uint32_t source = level - 1;
            recordImageBarrier(commands, imageBarrier(handle, VK_IMAGE_ASPECT_COLOR_BIT, ImageState::TransferDestination,
                                                      ImageState::TransferSource, ImageContents::Keep, source, 1));

            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, source, 0, 1};
            blit.srcOffsets[1] = {static_cast<std::int32_t>(std::max(width >> source, 1u)),
                                  static_cast<std::int32_t>(std::max(height >> source, 1u)), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            blit.dstOffsets[1] = {static_cast<std::int32_t>(std::max(width >> level, 1u)),
                                  static_cast<std::int32_t>(std::max(height >> level, 1u)), 1};
            vkCmdBlitImage(commands, handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);

            recordImageBarrier(commands, imageBarrier(handle, VK_IMAGE_ASPECT_COLOR_BIT, ImageState::TransferSource,
                                                      ImageState::FragmentRead, ImageContents::Keep, source, 1));
        }

        recordImageBarrier(commands, imageBarrier(handle, VK_IMAGE_ASPECT_COLOR_BIT, ImageState::TransferDestination,
                                                  ImageState::FragmentRead, ImageContents::Keep, levels - 1, 1));
    });

    image.setState(ImageState::FragmentRead);
}

VkImageView VulkanTexture::getView() const {
    return image.getView();
}

bool VulkanTexture::isSurface() const {
    return surface;
}
