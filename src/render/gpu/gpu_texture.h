#pragma once

#include "scene/asset_handles.h"

#include <SDL3/SDL.h>
#include <span>

// What a texture holds and how it is read.
struct GpuTextureDescription {
    SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    Uint32 bytesPerTexel = 4;

    // A surface's image gets a full mip chain, repeats past 0..1 and is filtered anisotropically. Otherwise it is a
    // lookup table: one level, clamped at its edges, blended between the four nearest texels.
    bool surface = true;
};

// An RGBA image with 8 bits per channel for a surface: colour (sRGB, decoded as it is sampled) or plain numbers (linear).
GpuTextureDescription surfaceTextureDescription(TextureEncoding encoding);

class GpuTexture {
public:
    // Upload width * height texels of description.bytesPerTexel bytes each, row by row.
    GpuTexture(
        SDL_GPUDevice* device,
        Uint32 width,
        Uint32 height,
        std::span<const Uint8> texels,
        const GpuTextureDescription& description
    );

    ~GpuTexture();

    // GPU resources must have a single owner.
    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;

    SDL_GPUTexture* getTexture() const;
    SDL_GPUSampler* getSampler() const;

private:
    // Borrowed device: it must outlive this texture.
    SDL_GPUDevice* device = nullptr;

    SDL_GPUTexture* texture = nullptr;
    SDL_GPUSampler* sampler = nullptr;

    void cleanup() noexcept;
};