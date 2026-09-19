#pragma once

#include "mat4.h"
#include "gpu_mesh.h"
#include "pixel.h"

#include <SDL3/SDL.h>

class GpuDisplay {
public:
    GpuDisplay(const char* title, int width, int height);
    ~GpuDisplay();

    // GPU resources must have a single owner.
    GpuDisplay(const GpuDisplay&) = delete;
    GpuDisplay& operator=(const GpuDisplay&) = delete;

    bool processEvents();

    // Clear colour and depth once. Returns false if no frame is available.
    // Background colour components range from 0.0f to 1.0f.
    bool beginFrame(float red, float green, float blue);

    // Draw into the active frame.
        void drawMesh(const GpuMesh& mesh, const Mat4& model, const Mat4& viewProjection, Pixel colour);

    // Finish and present the active frame.
    void endFrame();

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

    SDL_GPUCommandBuffer* commands = nullptr;
    SDL_GPURenderPass* pass = nullptr;
};