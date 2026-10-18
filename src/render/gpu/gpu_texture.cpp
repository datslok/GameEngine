#include "render/gpu/gpu_texture.h"
#include "render/gpu/mip_levels.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
    std::runtime_error textureGpuError(const char* message) {
        return std::runtime_error(
            std::string{message} + ": " + SDL_GetError()
        );
    }
}

/*
* Image files store colour sRGB-encoded. An _SRGB format makes the GPU decode texels to linear light when sampling (and
* when averaging mip levels), so lighting maths is done on real amounts of light. Data (roughness, metallic) is read
* exactly as stored, so it uses the plain format.
*/
GpuTextureDescription surfaceTextureDescription(TextureEncoding encoding) {
    GpuTextureDescription description;
    description.format = encoding == TextureEncoding::Srgb ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    return description;
}

GpuTexture::GpuTexture(
    SDL_GPUDevice* device,
    Uint32 width,
    Uint32 height,
    std::span<const Uint8> texels,
    const GpuTextureDescription& description
):
    device(device)
{
    if (device == nullptr) {
        throw std::invalid_argument("GpuTexture requires a GPU device");
    }

    if (width == 0 || height == 0) {
        throw std::invalid_argument("Texture dimensions must be positive");
    }

    // SDL transfer buffer sizes use Uint32.
    // Check before multiplying by the texel size to avoid overflow.
    const Uint64 texelCount = static_cast<Uint64>(width) * height;

    if (description.bytesPerTexel == 0 || texelCount > std::numeric_limits<Uint32>::max() / description.bytesPerTexel) {
        throw std::overflow_error("Texture upload is too large");
    }

    const Uint32 byteCount = static_cast<Uint32>(texelCount * description.bytesPerTexel);

    if (texels.size() != byteCount) {
        throw std::invalid_argument(
            "Texture data does not match its size and texel format"
        );
    }

    // Surfaces get the full chain, down to 1x1; a lookup table is read at its own size only.
    const Uint32 levelCount = description.surface ? mipLevelCount(width, height) : 1;

    SDL_GPUTransferBuffer* transfer = nullptr;
    SDL_GPUCommandBuffer* uploadCommands = nullptr;

    try {
        SDL_GPUTextureCreateInfo textureInfo{};
        textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
        textureInfo.format = description.format;
        // Generating mipmaps draws each smaller level from the one above, so a surface texture must also be a render target.
        textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (description.surface ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
        textureInfo.width = width;
        textureInfo.height = height;
        textureInfo.layer_count_or_depth = 1;
        textureInfo.num_levels = levelCount;
        textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;

        texture = SDL_CreateGPUTexture(device, &textureInfo);

        if (texture == nullptr) {
            throw textureGpuError("Texture creation failed");
        }

        SDL_GPUSamplerCreateInfo samplerInfo{};
        // Linear filtering blends the four nearest texels, so close-up textures are smooth instead of blocky.
        // Linear mipmap mode also blends the two nearest mip levels (trilinear), so there is no visible line where levels switch.
        samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        // Surfaces tile their image; a table holds its edge values beyond its range.
        const SDL_GPUSamplerAddressMode addressMode = description.surface ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_u = addressMode;
        samplerInfo.address_mode_v = addressMode;
        samplerInfo.address_mode_w = addressMode;

        // Surfaces seen at a grazing angle (the ground) cover a long thin strip of texture per pixel;
        // anisotropic filtering samples along the strip so they stay sharp instead of blurring.
        samplerInfo.enable_anisotropy = description.surface;
        samplerInfo.max_anisotropy = 16.0f;

        // Allow every mip level. The default maximum of 0 would lock sampling to the full-size level.
        samplerInfo.min_lod = 0.0f;
        samplerInfo.max_lod = static_cast<float>(levelCount - 1);

        sampler = SDL_CreateGPUSampler(device, &samplerInfo);

        if (sampler == nullptr) {
            throw textureGpuError("Sampler creation failed");
        }

        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = byteCount;

        transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);

        if (transfer == nullptr) {
            throw textureGpuError("Texture transfer buffer creation failed");
        }

        void* destination =
            SDL_MapGPUTransferBuffer(device, transfer, false);

        if (destination == nullptr) {
            throw textureGpuError("Texture transfer buffer mapping failed");
        }

        // Copy into SDL-owned upload memory.
        std::memcpy(destination, texels.data(), byteCount);
        SDL_UnmapGPUTransferBuffer(device, transfer);

        uploadCommands = SDL_AcquireGPUCommandBuffer(device);

        if (uploadCommands == nullptr) {
            throw textureGpuError("Texture upload command acquisition failed");
        }

        SDL_GPUCopyPass* copyPass =
            SDL_BeginGPUCopyPass(uploadCommands);

        if (copyPass == nullptr) {
            throw textureGpuError("Texture copy pass creation failed");
        }

        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = transfer;
        source.pixels_per_row = width;
        source.rows_per_layer = height;

        SDL_GPUTextureRegion target{};
        target.texture = texture;
        target.w = width;
        target.h = height;
        target.d = 1;

        SDL_UploadToGPUTexture(copyPass, &source, &target, false);
        SDL_EndGPUCopyPass(copyPass);

        // Each smaller level averages the one above, so distant surfaces read pre-filtered colours instead of shimmering.
        if (levelCount > 1) {
            SDL_GenerateMipmapsForGPUTexture(uploadCommands, texture);
        }

        const bool submitted =
            SDL_SubmitGPUCommandBuffer(uploadCommands);

        uploadCommands = nullptr;

        if (!submitted) {
            throw textureGpuError("Texture upload submission failed");
        }
    }
    catch (...) {
        if (uploadCommands != nullptr) {
            SDL_CancelGPUCommandBuffer(uploadCommands);
        }

        if (transfer != nullptr) {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }

        cleanup();
        throw;
    }

    SDL_ReleaseGPUTransferBuffer(device, transfer);
}

GpuTexture::~GpuTexture() {
    cleanup();
}

void GpuTexture::cleanup() noexcept {
    if (sampler != nullptr) {
        SDL_ReleaseGPUSampler(device, sampler);
        sampler = nullptr;
    }

    if (texture != nullptr) {
        SDL_ReleaseGPUTexture(device, texture);
        texture = nullptr;
    }
}

SDL_GPUTexture* GpuTexture::getTexture() const {
    return texture;
}

SDL_GPUSampler* GpuTexture::getSampler() const {
    return sampler;
}