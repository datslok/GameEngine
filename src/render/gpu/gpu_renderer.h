#pragma once

#include "math/mat4.h"
#include "render/gpu/gpu_mesh.h"
#include "render/gpu/gpu_texture.h"
#include "scene/asset_handles.h"
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

    // Clear colour and depth once. Returns false if no frame is available.
    // Background colour components range from 0.0f to 1.0f.
    bool beginFrame(float red, float green, float blue);

    // Upload the next mesh or texture. They are numbered in upload order, which must match the AssetManager's handles.
    // Call between frames, never between beginFrame() and endFrame().
    void uploadMesh(const IndexedMesh& mesh);
    void uploadTexture(Uint32 width, Uint32 height, std::span<const Uint8> pixels);

    std::size_t getMeshCount() const;
    std::size_t getTextureCount() const;

    // Send the frame's lights to the shader. Call between beginFrame() and endFrame(); beginFrame() starts with no lights.
    void setLighting(const FrameLighting& lighting);

    void drawMesh(MeshHandle mesh, const Mat4& model, const Mat4& viewProjection, const Material& material);

    // Finish and present the active frame.
    void endFrame();

    SDL_GPUDevice* getDevice() const;

    // Ask for a present mode. Falls back to Vsync when the GPU does not support it, and returns the mode now in use.
    PresentMode setPresentMode(PresentMode requested);
    PresentMode getPresentMode() const;

    // Read after beginFrame() returns true.
    float getFrameAspectRatio() const;

private:
    SDL_Window* window = nullptr;
    SDL_GPUDevice* device = nullptr;
    bool windowClaimed = false;
    PresentMode presentMode = PresentMode::Vsync;

    void cleanup() noexcept;

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    void createPipeline();

    SDL_GPUTexture* depthTexture = nullptr;
    Uint32 depthWidth = 0;
    Uint32 depthHeight = 0;

    void ensureDepthTexture(Uint32 width, Uint32 height);

    SDL_GPUCommandBuffer* commands = nullptr;
    SDL_GPURenderPass* pass = nullptr;

    // GPU copies, indexed exactly like MeshHandle and TextureHandle.
    std::vector<std::unique_ptr<GpuMesh>> meshes;
    std::vector<std::unique_ptr<GpuTexture>> textures;

    std::unique_ptr<GpuTexture> whiteTexture;
    void createWhiteTexture();
};
