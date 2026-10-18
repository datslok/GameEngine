#pragma once

#include "math/mat4.h"
#include "render/gpu/bloom_renderer.h"
#include "render/gpu/gpu_mesh.h"
#include "render/gpu/gpu_texture.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/render_settings.h"
#include "render/gpu/debug_line_renderer.h"
#include "render/gpu/display_output.h"
#include "render/gpu/shadow_cache.h"
#include "render/gpu/tone_mapping.h"
#include "render/render_stats.h"
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
    // The settings are read fresh each frame, so changes show at once.
    // Returns false if no frame was available (a minimized window), in which case nothing was drawn.
    bool render(const FrameDescription& frame, const RenderSettings& settings);

    // Upload the next mesh or texture. They are numbered in upload order, which must match the AssetManager's handles.
    // Textures are RGBA, four bytes per pixel, colour (sRGB) or data (linear).
    void uploadMesh(const IndexedMesh& mesh);
    void uploadTexture(Uint32 width, Uint32 height, std::span<const Uint8> pixels, TextureEncoding encoding);

    std::size_t getMeshCount() const;
    std::size_t getTextureCount() const;


    SDL_GPUDevice* getDevice() const;

    // Ask for a present mode. Falls back to Vsync when the GPU does not support it, and returns the mode now in use.
    PresentMode setPresentMode(PresentMode requested);
    PresentMode getPresentMode() const;

    const RenderStats& getLastFrameStats() const;

    // How the last frame reached the screen: SDR or HDR, with its peak and white level.
    const DisplayOutput& getDisplayOutput() const;

    // The aspect ratio of the last frame render() drew. Read after it returns true.
    float getFrameAspectRatio() const;

private:
    SDL_Window* window = nullptr;
    SDL_GPUDevice* device = nullptr;
    bool windowClaimed = false;
    PresentMode presentMode = PresentMode::Vsync;

    // SDR_LINEAR when supported: the swapchain encodes the shader's linear output as sRGB. HDR_EXTENDED_LINEAR (scRGB,
    // 16-bit floats) while HDR output is on.
    SDL_GPUSwapchainComposition swapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    SDL_GPUSwapchainComposition sdrComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    void useLinearSwapchain();
    void setSwapchainComposition(SDL_GPUSwapchainComposition composition);

    // HDR output: what the screen reported last frame, and how this frame reaches it.
    DisplayCapabilities displayCapabilities;
    DisplayOutput displayOutput;
    DisplayCapabilities readDisplayCapabilities() const;
    void updateDisplayOutput(const DisplaySettings& settings);

    void cleanup() noexcept;

    // The steps of render(), in order.
    SDL_GPUTexture* acquireFrame();
    void uploadShadowTiles(const std::vector<ShadowTileData>& tileData);
    void drawShadows(const FrameDescription& frame, const std::vector<ShadowTile>& tiles);
    void beginMainPass();
    void drawMesh(MeshHandle mesh, const Mat4& model, const Material& material);
    void beginToneMapPass(SDL_GPUTexture* swapchainTexture, const ToneMappingSettings& settings, const BloomComposite& bloomMix);
    void endFrame();
    void abandonFrame() noexcept;

    // Bind a mesh's vertex and index buffers to the current pass.
    const GpuMesh& bindMesh(MeshHandle mesh);

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    void createPipeline();

    // The main pass draws into these, both the size of the acquired frame: the lit scene in 16-bit floats, so brightness
    // above 1 survives until tone mapping (an 8-bit screen image would cut it off), and its depth.
    SDL_GPUTexture* sceneColour = nullptr;
    SDL_GPUTexture* depthTexture = nullptr;
    Uint32 frameWidth = 0;
    Uint32 frameHeight = 0;

    void ensureFrameTextures(Uint32 width, Uint32 height);

    // Between the main pass and tone mapping: the scene's scattered light, blurred into half-size float textures.
    std::unique_ptr<BloomRenderer> bloom;

    // The final pass: reads the scene image (exact texels, no filtering), mixes in the bloom and writes the tone mapped
    // screen image.
    SDL_GPUSampler* sceneSampler = nullptr;
    SDL_GPUGraphicsPipeline* toneMapPipeline = nullptr;
    void createToneMapResources();
    void createScreenPipelines();

    SDL_GPUCommandBuffer* commands = nullptr;
    SDL_GPURenderPass* pass = nullptr;

    // This frame's camera view and projection.
    Mat4 viewProjection = Mat4::identity();

    RenderStats stats;

    // Which lights had seats and shadows last frame, so similar lights do not swap them every frame.
    LightHistory lightHistory;

    // Shadows: the depth atlas (one tile per shadow view), a sampler that compares depths, and the depth-only pipeline that fills it.
    SDL_GPUTexture* shadowAtlas = nullptr;
    SDL_GPUSampler* shadowSampler = nullptr;
    SDL_GPUSampler* shadowDepthSampler = nullptr; // the atlas as raw depths, for soft shadows
    SDL_GPUGraphicsPipeline* shadowPipeline = nullptr;

    // Shadow caching: the atlas is kept between frames. Lights keep their squares (the layout), a tile is redrawn only
    // when its view or the casters in it changed (the memory), and a redrawn tile is first reset to the far depth.
    ShadowAtlasLayout shadowLayout;
    ShadowTileMemory shadowMemory;
    SDL_GPUGraphicsPipeline* shadowClearPipeline = nullptr;

    // Each tile's matrix, normal offset and place in the atlas, for the main shader (too big for a uniform block).
    SDL_GPUBuffer* shadowTileBuffer = nullptr;
    SDL_GPUTransferBuffer* shadowTileTransfer = nullptr;
    void createShadowResources();

    // Debug lines, drawn over the tone mapped screen image.
    std::unique_ptr<DebugLineRenderer> debugLines;

    // GPU copies, indexed exactly like MeshHandle and TextureHandle.
    std::vector<std::unique_ptr<GpuMesh>> meshes;
    std::vector<std::unique_ptr<GpuTexture>> textures;

    // Stands in for a missing texture: white multiplies a colour or a metallic-roughness factor by 1.
    std::unique_ptr<GpuTexture> whiteTexture;
    void createWhiteTexture();

    // The uploaded texture for a handle, or white for "none".
    const GpuTexture& textureOrWhite(TextureHandle handle) const;

    // Physically based shading's specular albedo table (render/gpu/brdf.h), integrated on the CPU once at start-up.
    std::unique_ptr<GpuTexture> brdfTable;
    void createBrdfTable();
};
