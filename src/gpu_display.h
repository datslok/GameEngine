#pragma once

#include "mat4.h"

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
    void drawQuad(const Mat4& transform, float red, float green, float blue);

private:
    SDL_Window* window = nullptr;
    SDL_GPUDevice* device = nullptr;

    bool videoInitialized = false;
    bool windowClaimed = false;

    void cleanup() noexcept;

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    void createPipeline();

    SDL_GPUBuffer* vertexBuffer = nullptr;
    void createGeometry();

    SDL_GPUBuffer* indexBuffer = nullptr;
};