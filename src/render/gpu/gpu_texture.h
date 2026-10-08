#pragma once

#include <SDL3/SDL.h>
#include <span>

class GpuTexture {
public:
    // Upload four bytes per pixel: red, green, blue, alpha.
    GpuTexture(
        SDL_GPUDevice* device,
        Uint32 width,
        Uint32 height,
        std::span<const Uint8> pixels
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