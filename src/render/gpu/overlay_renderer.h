#pragma once

#include <SDL3/SDL.h>

struct ImDrawData;

// The overlay image's format: 8 bits per channel, written as ImGui expects (sRGB values, blended as on an 8-bit screen).
inline constexpr SDL_GPUTextureFormat overlayFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

/*
* Draws the debug UI into its own transparent image, then lays that over the finished screen image, last of all. A
* separate image keeps the UI's look independent of the screen: on an HDR screen its colours are decoded and scaled to
* the white level (overlay_composite.h) instead of being shown at scRGB's dim 80 nits. Phase 9's HUD goes the same way.
*
* Each frame with UI: drawUi() outside any render pass, then composite() in the open screen pass. The composite pipeline
* is built for the screen's format, so the renderer remakes this whenever that changes (HDR on or off).
*/
class OverlayRenderer {
public:
    OverlayRenderer(SDL_GPUDevice* device, SDL_GPUTextureFormat screenFormat);
    ~OverlayRenderer();

    // GPU resources must have a single owner.
    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    // Upload ImGui's vertices and textures, then draw it into the overlay image, cleared first. Outside render passes.
    void drawUi(SDL_GPUCommandBuffer* commands, ImDrawData* drawData, Uint32 frameWidth, Uint32 frameHeight);

    // Blend the overlay over the open screen pass. whiteLevel is what SDR white is written as on this screen.
    void composite(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, float whiteLevel);

private:
    // Borrowed device: it must outlive this renderer.
    SDL_GPUDevice* device = nullptr;

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUSampler* sampler = nullptr;

    // The screen's size, remade when the window is resized.
    SDL_GPUTexture* image = nullptr;
    Uint32 imageWidth = 0;
    Uint32 imageHeight = 0;

    void createPipeline(SDL_GPUTextureFormat screenFormat);
    void ensureImage(Uint32 width, Uint32 height);
    void cleanup() noexcept;
};
