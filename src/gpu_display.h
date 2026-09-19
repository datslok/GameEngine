#pragma once

#include "mat4.h"
#include "gpu_mesh.h"

#include <SDL3/SDL.h>

class GpuDisplay {
public:
    GpuDisplay(const char* title, int width, int height);
    ~GpuDisplay();

    // GPU resources must have a single owner.
    GpuDisplay(const GpuDisplay&) = delete;
    GpuDisplay& operator=(const GpuDisplay&) = delete;

    bool processEvents();

    // Colour components range from 0.0f to 1.0f.
    void drawMesh(const GpuMesh& mesh, const Mat4& transform, float red, float green, float blue);

    SDL_GPUDevice* getDevice() const;

private:
    SDL_Window* window = nullptr;
    SDL_GPUDevice* device = nullptr;

    bool videoInitialized = false;
    bool windowClaimed = false;

    void cleanup() noexcept;

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    void createPipeline();

    SDL_GPUTexture* depthTexture = nullptr;
    Uint32 depthWidth = 0;
    Uint32 depthHeight = 0;

    void ensureDepthTexture(Uint32 width, Uint32 height);
};