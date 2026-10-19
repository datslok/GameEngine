#pragma once

#include "math/mat4.h"
#include "render/gpu/bloom.h"
#include "render/gpu/clusters.h"
#include "render/gpu/display_output.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/light_priority.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/shadow_atlas.h"
#include "render/gpu/shadow_cache.h"
#include "render/gpu/shadow_map.h"
#include "render/captured_frame.h"
#include "render/gpu/render_settings.h"
#include "render/frames_in_flight.h"
#include "render/present_mode.h"
#include "render/render_stats.h"
#include "render/vulkan/bindless_textures.h"
#include "render/vulkan/frame_context.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/scene_layout.h"
#include "render/vulkan/vulkan_bloom.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_debug_lines.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_mesh.h"
#include "render/vulkan/vulkan_overlay.h"
#include "render/vulkan/vulkan_pipeline.h"
#include "render/vulkan/vulkan_swapchain.h"
#include "render/vulkan/vulkan_texture.h"
#include "scene/asset_handles.h"
#include "scene/frame_description.h"
#include "scene/indexed_mesh.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct ImDrawData;

/*
* The engine's renderer, on Vulkan 1.3 (phase 6b; it replaced SDL_GPU, drawing every golden image bit for bit the same):
* the shadow atlas, the main pass into a float scene image, bloom, tone mapping into the swapchain image, debug lines
* and the debug UI's overlay. It draws into a window it does not own, so the window must outlive it. It says itself when
* each image changes from being written to being read (the barriers in each pass), which work goes to the GPU when,
* and how the CPU waits for it.
*
* Each frame: wait until the GPU has finished the last frame that used this frame's resources (a fence: with two frames
* in flight that is the frame before last, so the CPU records while the GPU still draws the previous one), acquire a
* swapchain image (a semaphore signals when it is free), record every pass into one command buffer, submit it (waiting
* for that semaphore, signalling another when done), and present (waiting for the second semaphore).
*/
class VulkanRenderer {
public:
    // validation checks every graphics call (EngineSettings::gpuValidation): slow, for development. framesInFlight
    // as setFramesInFlight takes it.
    VulkanRenderer(SDL_Window* window, bool validation, std::uint32_t framesInFlight);
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    // Draw and present one whole frame. The frame's camera gets the acquired image's aspect ratio (the caller's camera
    // is not changed); the settings are read fresh each frame. ui is the debug UI's finished frame (ImGui::GetDrawData()),
    // drawn over everything, or null. Returns false, drawing nothing, when no image was available (a minimised window).
    bool render(const FrameDescription& frame, const RenderSettings& settings, ImDrawData* ui = nullptr);

    // The same frame drawn off screen at width x height for a fixed display output, and read back (engine/golden.h): the
    // scene image and the tone mapped image as that display would get it. Presents nothing; waits for the GPU.
    CapturedFrame capture(const FrameDescription& frame, const RenderSettings& settings, Uint32 width, Uint32 height,
                          const DisplayOutput& output);

    // The GPU's name as the driver reports it, for keeping golden images per GPU.
    std::string getDeviceName() const;

    // Upload the next mesh or texture. They are numbered in upload order, which must match the AssetManager's handles.
    // Textures are RGBA, four bytes per pixel, colour (sRGB) or data (linear). Between frames only.
    void uploadMesh(const IndexedMesh& mesh);
    void uploadTexture(Uint32 width, Uint32 height, std::span<const Uint8> pixels, TextureEncoding encoding);
    std::size_t getMeshCount() const;
    std::size_t getTextureCount() const;

    // The debug UI's drawing (ImGui's Vulkan backend), into the overlay image. Start it once an ImGui context exists,
    // call beginDebugUiFrame() before each ImGui::NewFrame(), and stop it before the context is destroyed.
    void startDebugUi();
    void beginDebugUiFrame();
    void stopDebugUi();

    // Ask for a present mode. Falls back to Vsync when the window does not support it, and returns the mode now in use.
    PresentMode setPresentMode(PresentMode requested);
    PresentMode getPresentMode() const;

    // How many frames the CPU may record ahead of the GPU (render/frames_in_flight.h), clamped to 1..maxFramesInFlight;
    // returns the count now in use. A change waits for the GPU, remakes every frame's own resources and rebuilds the
    // swapchain before the next frame. Between frames only.
    std::uint32_t setFramesInFlight(std::uint32_t requested);
    std::uint32_t getFramesInFlight() const;

    const RenderStats& getLastFrameStats() const;

    // How the last frame reached the screen: SDR or HDR, with its peak and white level.
    const DisplayOutput& getDisplayOutput() const;

    // The aspect ratio of the last frame render() drew. Read after it returns true.
    float getFrameAspectRatio() const;

private:
    SDL_Window* window = nullptr;

    // Declared first, so it is destroyed last: everything below is made from it.
    VulkanDevice device;
    DescriptorLayouts layouts;
    ImmediateCommands uploads;
    VulkanSwapchain swapchain;

    // What one frame in flight needs for itself.
    struct Frame {
        OwnedHandle<VkCommandPool> commandPool;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        OwnedHandle<VkFence> finished;
        OwnedHandle<VkSemaphore> imageAcquired;
        std::unique_ptr<FrameDescriptors> descriptors;
        std::unique_ptr<FrameUniforms> uniforms;
        std::unique_ptr<SceneBuffers> scene; // the GPU scene: instances, materials, lights, shadow tiles and casters

        // GPU timestamps around each pass (none without timestamp support): how many this frame's commands have
        // written so far, and whether the last submission wrote them all, so they can be read once its fence passes.
        OwnedHandle<VkQueryPool> timestamps;
        std::uint32_t timestampsWritten = 0;
        bool timestampsComplete = false;
    };

    // One per frame in flight; the swapchain was built for swapchainFramesInFlight of them (rebuilt when they differ).
    std::vector<Frame> frames;
    std::uint32_t frameIndex = 0;
    std::uint32_t framesInFlight = 1;
    std::uint32_t swapchainFramesInFlight = 0;

    // The frame being recorded: its command buffer is open, and if it draws to the screen, the image it acquired.
    bool recording = false;
    bool renderingOpen = false;
    std::optional<std::uint32_t> acquiredImage;

    PresentMode presentMode = PresentMode::Vsync;
    DisplayCapabilities displayCapabilities;
    DisplayOutput displayOutput;

    // Samplers shared by everything that reads the same way.
    OwnedHandle<VkSampler> surfaceSampler;     // surfaces: trilinear, anisotropic, repeating
    OwnedHandle<VkSampler> tableSampler;       // lookup tables: bilinear, clamped
    OwnedHandle<VkSampler> nearestSampler;     // exact texels, clamped (the scene for tone mapping, the overlay)
    OwnedHandle<VkSampler> linearSampler;      // bilinear, clamped, one level (bloom)
    OwnedHandle<VkSampler> shadowSampler;      // compares depths, blending the four nearest results
    OwnedHandle<VkSampler> shadowDepthSampler; // raw depths, exact texels (the soft shadow blocker search)

    // The scene pipelines' descriptors: every texture in one array, and the layout of the frame's scene set. Before the
    // pipelines, which use the layout, so they go first.
    std::unique_ptr<BindlessTextures> bindlessTextures;
    std::unique_ptr<SceneLayout> sceneLayout;

    // The main pass alone (finding the nearest surface itself) and after the depth prepass (shading only it), and the prepass.
    std::unique_ptr<VulkanPipeline> mainPipeline;
    std::unique_ptr<VulkanPipeline> mainAfterPrepassPipeline;
    std::unique_ptr<VulkanPipeline> depthPrepassPipeline;
    std::unique_ptr<VulkanPipeline> shadowPipeline;
    std::unique_ptr<VulkanPipeline> shadowClearPipeline;
    std::unique_ptr<VulkanPipeline> toneMapPipeline;
    VkFormat screenFormat = VK_FORMAT_UNDEFINED;

    // Clustered shading (render/gpu/clusters.h): the compute pass that lists which lights may reach each cluster, its
    // grid for this frame, and the lists, one bit per light per cluster, sized for the frame like the images. Only the
    // GPU writes and reads them, within a frame, so there is one copy, ordered by barriers rather than one per frame.
    std::unique_ptr<ComputePipeline> clusterLightsPipeline;
    ClusterGrid clusterGrid;
    VulkanBuffer clusterMasks;

    // The main pass draws into these, the size of the frame: the lit scene in 16-bit floats, and its depth.
    VulkanImage sceneColour;
    VulkanImage depth;
    std::uint32_t frameWidth = 0;
    std::uint32_t frameHeight = 0;

    // Shadows: every shadow view in one depth atlas, kept between frames (tiles are redrawn only when they change).
    // Counts frames for the memory allocator, which refreshes the driver's memory figures when it changes.
    std::uint32_t memoryFrameNumber = 0;

    VulkanImage shadowAtlas;
    ShadowAtlasLayout shadowLayout;
    ShadowTileMemory shadowMemory;
    LightHistory lightHistory;

    std::unique_ptr<VulkanBloom> bloom;
    std::unique_ptr<VulkanDebugLines> debugLines;
    std::unique_ptr<VulkanOverlay> overlay;
    bool debugUiStarted = false;

    // GPU copies, indexed exactly like MeshHandle and TextureHandle.
    std::vector<std::unique_ptr<VulkanMesh>> meshes;
    std::vector<std::unique_ptr<VulkanTexture>> textures;
    std::unique_ptr<VulkanTexture> whiteTexture;
    std::unique_ptr<VulkanTexture> brdfTable;

    // This frame's camera in world space (culling, debug lines) and measured from the camera (the main pass).
    Mat4 viewProjection = Mat4::identity();
    Mat4 relativeViewProjection = Mat4::identity();
    Vec3 cameraOrigin{0.0f, 0.0f, 0.0f};

    RenderStats stats;

    // The last frame's GPU timings read back and not yet handed to stats (each frame's are reported once).
    GpuTimings unreportedGpuTimings;

    void checkFormats() const;
    void createFrames();
    void createSamplers();
    void createSceneLayout();
    void createPipelines();
    void createScreenPipelines(VkFormat format);
    std::unique_ptr<VulkanPipeline> createToneMapPipeline(VkFormat format);
    void createShadowAtlas(const ShadowAtlasSize& size);
    void ensureShadowAtlas(const ShadowAtlasSize& size);
    void createFixedTextures();
    void ensureFrameTextures(std::uint32_t width, std::uint32_t height);

    void updateDisplayOutput(const DisplaySettings& settings);
    bool prepareSwapchain();

    FrameContext beginFrame();
    void submitFrame(FrameContext& frame, bool toScreen);
    void abandonFrame() noexcept;

    // Timing and labelling passes: a timestamp and a debug label at each pass's start and end, in GpuPass order.
    void readGpuTimings(Frame& frame);
    void writeTimestampsThrough(FrameContext& frame, std::uint32_t index);
    void beginGpuPass(FrameContext& frame, GpuPass pass);
    void endGpuPass(FrameContext& frame, GpuPass pass);
    void finishGpuTimings(FrameContext& frame);

    BloomComposite drawScene(FrameContext& frame, const FrameDescription& description, const RenderSettings& settings, ImDrawData* ui);
    // The shadow tiles to redraw this frame, each with its casters batched by mesh, and the shadow part of the draw list:
    // each caster's instance number, one tile after another, following the main pass's part.
    struct ShadowTileDraw {
        const ShadowTile* tile = nullptr;
        BatchedDraws casters;
        std::uint64_t signature = 0;
    };

    struct ShadowDraws {
        std::vector<ShadowTileDraw> tiles;
        std::vector<std::uint32_t> drawList;
    };

    VkDescriptorSet writeScene(FrameContext& frame, const FrameDescription& description, const GpuLights& lights, const ShadowPlan& shadows,
                               const BatchedDraws& mainDraws, const ShadowDraws& shadowDraws, const RenderSettings& settings);
    void buildClusterLists(FrameContext& frame, VkDescriptorSet sceneSet);
    ShadowDraws prepareShadowDraws(const FrameDescription& description, const std::vector<ShadowTile>& tiles, std::uint32_t firstEntry);
    void drawShadows(FrameContext& frame, const ShadowDraws& shadowDraws, VkDescriptorSet sceneSet);
    void drawDepthPrepass(FrameContext& frame, const BatchedDraws& mainDraws, VkDescriptorSet sceneSet);
    void drawMainPass(FrameContext& frame, const BatchedDraws& mainDraws, VkDescriptorSet sceneSet, bool afterPrepass,
                      const Vec3& background);
    std::size_t drawBatches(VkCommandBuffer commands, const std::vector<DrawBatch>& batches) const;
    void drawToneMap(FrameContext& frame, const VulkanPipeline& pipeline, const ToneMappingSettings& settings, const BloomComposite& bloomMix);

    void bindMesh(VkCommandBuffer commands, MeshHandle mesh, std::uint32_t& indexCount) const;
    void checkTexturesUploaded(const Material& material) const;
    SampledImage sampled(const VulkanTexture& texture) const;
};
