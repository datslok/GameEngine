#pragma once

#include "math/mat4.h"
#include "render/gpu/debug_line_vertices.h"
#include "scene/debug_draw.h"

#include <SDL3/SDL.h>
#include <vector>

/*
* Draws a frame's debug lines on top of the finished scene: world lines through the camera, screen lines in window pixels.
* Owns its pipeline and a vertex buffer that grows when a frame has more lines than it holds.
* Each frame: upload() before any render pass (copies cannot happen inside one), then draw() at the end of the main pass.
*/
class DebugLineRenderer {
public:
    // Builds the pipeline for colour targets of this format (the swapchain's) with a D32 depth target.
    DebugLineRenderer(SDL_GPUDevice* device, SDL_GPUTextureFormat colourFormat);
    ~DebugLineRenderer();

    // GPU resources must have a single owner.
    DebugLineRenderer(const DebugLineRenderer&) = delete;
    DebugLineRenderer& operator=(const DebugLineRenderer&) = delete;

    // Copy this frame's lines to the GPU, in their own copy pass. Must be called outside render passes.
    void upload(SDL_GPUCommandBuffer* commands, const std::vector<DebugLine>& worldLines, const std::vector<DebugLine>& screenLines);

    // Draw the lines from the last upload into the open pass. Screen lines map (0, 0) to the top-left of a frame this size.
    void draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& viewProjection, Uint32 frameWidth, Uint32 frameHeight);

private:
    // Borrowed device: it must outlive this renderer.
    SDL_GPUDevice* device = nullptr;

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUBuffer* vertexBuffer = nullptr;
    SDL_GPUTransferBuffer* transferBuffer = nullptr;
    Uint32 capacity = 0; // in vertices

    // What the last upload holds: world line vertices first, then screen line vertices.
    Uint32 worldVertexCount = 0;
    Uint32 screenVertexCount = 0;

    void createPipeline(SDL_GPUTextureFormat colourFormat);
    void ensureCapacity(Uint32 vertexCount);
    void drawRange(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& transform, Uint32 firstVertex, Uint32 vertexCount);
    void cleanup() noexcept;
};
