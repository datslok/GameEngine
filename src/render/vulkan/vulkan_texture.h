#pragma once

#include "core/coverage_mips.h"
#include "render/vulkan/vulkan_memory.h"
#include "scene/asset_handles.h"
#include "scene/image_data.h"

#include <cstdint>
#include <span>

class ImmediateCommands;
class VulkanDevice;

// What a texture holds: its format, its bytes per texel, and whether it is a surface's image or a lookup table.
struct VulkanTextureDescription {
    VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    std::uint32_t bytesPerTexel = 4;

    // A surface's image gets a full mip chain (read with the renderer's repeating, anisotropic sampler); a lookup table
    // has one level (read with the clamping sampler).
    bool surface = true;
};

// An RGBA image with 8 bits per channel for a surface: colour (sRGB, decoded to linear as it is sampled) or data (linear).
VulkanTextureDescription surfaceTexture(TextureEncoding encoding);

// The BC7 format for a surface's compressed texture: colour (sRGB) or data (linear), decoded as RGBA8 would be.
VkFormat compressedSurfaceFormat(TextureEncoding encoding);

/*
* A texture in GPU memory, ready for fragment shaders to sample. A surface's smaller mip levels are made on the GPU, each
* by a filtered copy (a blit) of the level above, as SDL_GPU made them, so distant surfaces read pre-averaged colours,
* unless they come built (mips: every level below the full size, as a cut-out's are, core/coverage_mips.h).
*/
class VulkanTexture {
public:
    VulkanTexture(const VulkanDevice& device, ImmediateCommands& uploads, std::uint32_t width, std::uint32_t height,
                  std::span<const std::uint8_t> texels, const VulkanTextureDescription& description, std::span<const ImageLevel> mips = {});

    // A surface's texture already BC7-compressed, every level built (assets/texture_cooking.h), each copied in as it is.
    VulkanTexture(const VulkanDevice& device, ImmediateCommands& uploads, const CompressedTexture& compressed);

    VkImageView getView() const;
    bool isSurface() const;

private:
    VulkanImage image;
    bool surface = true;
};
