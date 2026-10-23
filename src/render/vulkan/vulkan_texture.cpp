#include "render/vulkan/vulkan_texture.h"
#include "render/gpu/mip_levels.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

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
* The upload: level 0 is copied in from an upload buffer, then each smaller level is blitted from the one above
* (generateMipmaps), and every level is left for shaders to read. Levels built on the CPU follow level 0 in the upload
* buffer and are copied in too, one region each, instead of blitted.
*/
VulkanTexture::VulkanTexture(const VulkanDevice& device, ImmediateCommands& uploads, std::uint32_t width, std::uint32_t height,
                             std::span<const std::uint8_t> texels, const VulkanTextureDescription& description, std::span<const ImageLevel> mips):
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
    std::uint64_t uploadBytes = byteCount;

    if (!mips.empty()) {
        if (mips.size() + 1 != levels || description.bytesPerTexel != 4) {
            throw std::invalid_argument("A texture's built mip levels must be every RGBA8 level below its full size");
        }

        for (std::size_t level = 0; level < mips.size(); ++level) {
            const std::uint32_t levelWidth = std::max(width >> (level + 1), 1u);
            const std::uint32_t levelHeight = std::max(height >> (level + 1), 1u);

            if (mips[level].width != levelWidth || mips[level].height != levelHeight ||
                mips[level].pixels.size() != std::size_t{levelWidth} * levelHeight * 4) {
                throw std::invalid_argument("A built mip level does not have its level's size");
            }

            uploadBytes += mips[level].pixels.size();
        }
    }

    ImageDescription imageDescription;
    imageDescription.format = description.format;
    imageDescription.width = width;
    imageDescription.height = height;
    imageDescription.levels = levels;
    imageDescription.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (levels > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    image = VulkanImage{device, imageDescription};

    VulkanBuffer staging{device.getAllocator(), uploadBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Upload};
    auto* mapped = static_cast<std::uint8_t*>(staging.getMapped());
    std::memcpy(mapped, texels.data(), texels.size());

    std::vector<VkBufferImageCopy> copies(1);
    copies[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copies[0].imageSubresource.layerCount = 1;
    copies[0].imageExtent = {width, height, 1};
    VkDeviceSize offset = byteCount;

    for (std::size_t level = 0; level < mips.size(); ++level) {
        std::memcpy(mapped + offset, mips[level].pixels.data(), mips[level].pixels.size());
        VkBufferImageCopy copy = copies[0];
        copy.bufferOffset = offset;
        copy.imageSubresource.mipLevel = static_cast<std::uint32_t>(level + 1);
        copy.imageExtent = {mips[level].width, mips[level].height, 1};
        copies.push_back(copy);
        offset += mips[level].pixels.size();
    }

    staging.flush();

    uploads.run([&](VkCommandBuffer commands) {
        const VkImage handle = image.get();
        recordImageBarrier(commands, imageBarrier(handle, VK_IMAGE_ASPECT_COLOR_BIT, ImageState::Undefined,
                                                  ImageState::TransferDestination, ImageContents::Discard));
        vkCmdCopyBufferToImage(commands, staging.get(), handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(copies.size()),
                               copies.data());

        if (mips.empty()) {
            generateMipmaps(commands, image, ImageState::TransferDestination, ImageState::SurfaceRead);
        } else {
            // Every level was copied in: all of them go from the copy's writes to shaders' reads at once.
            recordImageBarrier(commands, imageBarrier(handle, VK_IMAGE_ASPECT_COLOR_BIT, ImageState::TransferDestination, ImageState::SurfaceRead,
                                                      ImageContents::Keep));
            image.setState(ImageState::SurfaceRead);
        }
    });
}

VkImageView VulkanTexture::getView() const {
    return image.getView();
}

bool VulkanTexture::isSurface() const {
    return surface;
}
