#include "gpu_texture.h"

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

GpuTexture::GpuTexture(
    SDL_GPUDevice* device,
    Uint32 width,
    Uint32 height,
    std::span<const Uint8> pixels
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
    // Check before multiplying by four to avoid overflow.
    const Uint64 pixelCount = static_cast<Uint64>(width) * height;

    if (pixelCount > std::numeric_limits<Uint32>::max() / 4) {
        throw std::overflow_error("Texture upload is too large");
    }

    const Uint32 byteCount = static_cast<Uint32>(pixelCount * 4);

    if (pixels.size() != byteCount) {
        throw std::invalid_argument(
            "Texture requires exactly four bytes per pixel"
        );
    }

    SDL_GPUTransferBuffer* transfer = nullptr;
    SDL_GPUCommandBuffer* uploadCommands = nullptr;

    try {
        SDL_GPUTextureCreateInfo textureInfo{};
        textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
        textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        textureInfo.width = width;
        textureInfo.height = height;
        textureInfo.layer_count_or_depth = 1;
        textureInfo.num_levels = 1;
        textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;

        texture = SDL_CreateGPUTexture(device, &textureInfo);

        if (texture == nullptr) {
            throw textureGpuError("Texture creation failed");
        }

        SDL_GPUSamplerCreateInfo samplerInfo{};
        samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
        samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
        samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;

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
        std::memcpy(destination, pixels.data(), byteCount);
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