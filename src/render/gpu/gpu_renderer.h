#pragma once

#include "math/mat4.h"
#include "render/gpu/gpu_mesh.h"
#include "render/gpu/gpu_texture.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/debug_line_vertices.h"
#include "render/gpu/shadow_map.h"
#include "scene/asset_handles.h"
#include "scene/camera.h"
#include "scene/frame_description.h"
#include "scene/material.h"
#include "scene/indexed_mesh.h"
#include "scene/lighting.h"

#include <SDL3/SDL.h>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

/*
* How finished frames reach the screen.
* Vsync: each frame waits its turn for a screen refresh. No tearing, but queued frames add latency. Always supported.
* Mailbox: no tearing, and a newer frame replaces one still waiting, so the screen shows the freshest frame. Low latency.
* Immediate: frames are shown as soon as they are ready. Lowest latency, but the image can tear.
*/
enum class PresentMode {
    Vsync,
    Mailbox,
    Immediate
};

/*
* Draws meshes with SDL's GPU API (Vulkan backend) into a window it does not own.
* The window must outlive the renderer, so declare the window first wherever both are members.
*/
class GpuRenderer {
public:
    explicit GpuRenderer(SDL_Window* window);
    ~GpuRenderer();

    // GPU resources must have a single owner.
    GpuRenderer(const GpuRenderer&) = delete;
    GpuRenderer& operator=(const GpuRenderer&) = delete;

    // Draw and present one whole frame. The frame's camera gets the acquired frame's aspect ratio (the caller's camera is not changed).
    // Returns false if no frame was available (a minimized window), in which case nothing was drawn.
    bool render(const FrameDescription& frame);

    // Upload the next mesh or texture. They are numbered in upload order, which must match the AssetManager's handles.
    void uploadMesh(const IndexedMesh& mesh);
    void uploadTexture(Uint32 width, Uint32 height, std::span<const Uint8> pixels);

    std::size_t getMeshCount() const;
    std::size_t getTextureCount() const;


    SDL_GPUDevice* getDevice() const;

    // Ask for a present mode. Falls back to Vsync when the GPU does not support it, and returns the mode now in use.
    PresentMode setPresentMode(PresentMode requested);
    PresentMode getPresentMode() const;

    // The aspect ratio of the last frame render() drew. Read after it returns true.
    float getFrameAspectRatio() const;

private:
    SDL_Window* window = nullptr;
    SDL_GPUDevice* device = nullptr;
    bool windowClaimed = false;
    PresentMode presentMode = PresentMode::Vsync;

    // SDR_LINEAR when supported: the swapchain encodes the shader's linear output as sRGB.
    SDL_GPUSwapchainComposition swapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    void useLinearSwapchain();

    void cleanup() noexcept;

    // The steps of render(), in order.
    SDL_GPUTexture* acquireFrame();
    void drawShadows(const FrameDescription& frame, const std::vector<Mat4>& tileMatrices);
    void beginMainPass(SDL_GPUTexture* swapchainTexture);
    void drawMesh(MeshHandle mesh, const Mat4& model, const Material& material);
    void endFrame();
    void abandonFrame() noexcept;

    // Bind a mesh's vertex and index buffers to the current pass.
    const GpuMesh& bindMesh(MeshHandle mesh);

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    void createPipeline();

    SDL_GPUTexture* depthTexture = nullptr;
    Uint32 depthWidth = 0;
    Uint32 depthHeight = 0;

    void ensureDepthTexture(Uint32 width, Uint32 height);

    SDL_GPUCommandBuffer* commands = nullptr;
    SDL_GPURenderPass* pass = nullptr;

    // This frame's camera view and projection.
    Mat4 viewProjection = Mat4::identity();

    // Shadows: the depth atlas (one tile per shadow view), a sampler that compares depths, and the depth-only pipeline that fills it.
    SDL_GPUTexture* shadowAtlas = nullptr;
    SDL_GPUSampler* shadowSampler = nullptr;
    SDL_GPUGraphicsPipeline* shadowPipeline = nullptr;
    void createShadowResources();

    // Debug lines: a pipeline that draws a line list on top of everything, and a vertex buffer (with its upload buffer)
    // that grows when a frame has more lines than it holds.
    SDL_GPUGraphicsPipeline* debugLinePipeline = nullptr;
    SDL_GPUBuffer* debugLineBuffer = nullptr;
    SDL_GPUTransferBuffer* debugLineTransfer = nullptr;
    Uint32 debugLineCapacity = 0; // in vertices
    void createDebugLinePipeline();
    void uploadDebugLines(const std::vector<DebugLineVertex>& vertices);
    void drawDebugLines(Uint32 vertexCount);

    // GPU copies, indexed exactly like MeshHandle and TextureHandle.
    std::vector<std::unique_ptr<GpuMesh>> meshes;
    std::vector<std::unique_ptr<GpuTexture>> textures;

    std::unique_ptr<GpuTexture> whiteTexture;
    void createWhiteTexture();
};
