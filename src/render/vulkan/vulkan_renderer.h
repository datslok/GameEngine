#pragma once

#include "math/mat4.h"
#include "render/gpu/bloom.h"
#include "render/gpu/display_output.h"
#include "render/gpu/light_priority.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/shadow_atlas.h"
#include "render/gpu/shadow_cache.h"
#include "render/gpu/shadow_map.h"
#include "render/render_backend.h"
#include "render/vulkan/frame_context.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_bloom.h"
#include "render/vulkan/vulkan_debug_lines.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_mesh.h"
#include "render/vulkan/vulkan_overlay.h"
#include "render/vulkan/vulkan_pipeline.h"
#include "render/vulkan/vulkan_swapchain.h"
#include "render/vulkan/vulkan_texture.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

/*
* How many frames the CPU may prepare ahead of the GPU. One, as SDL_GPU was set up: the CPU waits for the last frame to
* finish before recording the next, so input is never more than a frame old. Becomes an EngineSettings value next in
* phase 6b (2 lets the CPU and GPU work at the same time).
*/
inline constexpr std::uint32_t vulkanFramesInFlight = 1;

/*
* The engine's own Vulkan 1.3 renderer (phase 6b), drawing exactly what GpuRenderer draws with the same shaders: the
* shadow atlas, the main pass into a float scene image, bloom, tone mapping into the swapchain image, debug lines and
* the debug UI's overlay. Unlike SDL_GPU it says itself when each image changes from being written to being read (the
* barriers in each pass), which work goes to the GPU when, and how the CPU waits for it.
*
* Each frame: wait until the GPU has finished the last frame that used this frame's resources (a fence), acquire a
* swapchain image (a semaphore signals when it is free), record every pass into one command buffer, submit it (waiting
* for that semaphore, signalling another when done), and present (waiting for the second semaphore).
*/
class VulkanRenderer final : public RenderBackend {
public:
    VulkanRenderer(SDL_Window* window, bool validation);
    ~VulkanRenderer() override;

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    bool render(const FrameDescription& frame, const RenderSettings& settings, ImDrawData* ui = nullptr) override;
    CapturedFrame capture(const FrameDescription& frame, const RenderSettings& settings, Uint32 width, Uint32 height,
                          const DisplayOutput& output) override;
    std::string getDeviceName() const override;

    void uploadMesh(const IndexedMesh& mesh) override;
    void uploadTexture(Uint32 width, Uint32 height, std::span<const Uint8> pixels, TextureEncoding encoding) override;
    std::size_t getMeshCount() const override;
    std::size_t getTextureCount() const override;

    void startDebugUi() override;
    void beginDebugUiFrame() override;
    void stopDebugUi() override;

    PresentMode setPresentMode(PresentMode requested) override;
    PresentMode getPresentMode() const override;

    const RenderStats& getLastFrameStats() const override;
    const DisplayOutput& getDisplayOutput() const override;
    float getFrameAspectRatio() const override;

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
        VulkanBuffer shadowTiles; // each shadow tile's matrix and place in the atlas, read by the main pass
    };

    std::vector<Frame> frames;
    std::uint32_t frameIndex = 0;

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

    std::unique_ptr<VulkanPipeline> mainPipeline;
    std::unique_ptr<VulkanPipeline> shadowPipeline;
    std::unique_ptr<VulkanPipeline> shadowClearPipeline;
    std::unique_ptr<VulkanPipeline> toneMapPipeline;
    VkFormat screenFormat = VK_FORMAT_UNDEFINED;

    // The main pass draws into these, the size of the frame: the lit scene in 16-bit floats, and its depth.
    VulkanImage sceneColour;
    VulkanImage depth;
    std::uint32_t frameWidth = 0;
    std::uint32_t frameHeight = 0;

    // Shadows: every shadow view in one depth atlas, kept between frames (tiles are redrawn only when they change).
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

    void checkFormats() const;
    void createFrames();
    void createSamplers();
    void createPipelines();
    void createScreenPipelines(VkFormat format);
    std::unique_ptr<VulkanPipeline> createToneMapPipeline(VkFormat format);
    void createShadowAtlas();
    void createFixedTextures();
    void ensureFrameTextures(std::uint32_t width, std::uint32_t height);

    void updateDisplayOutput(const DisplaySettings& settings);
    bool prepareSwapchain();

    FrameContext beginFrame();
    void submitFrame(FrameContext& frame, bool toScreen);
    void abandonFrame() noexcept;

    BloomComposite drawScene(FrameContext& frame, const FrameDescription& description, const RenderSettings& settings, ImDrawData* ui);
    void drawShadows(FrameContext& frame, const FrameDescription& description, const std::vector<ShadowTile>& tiles);
    void drawMainPass(FrameContext& frame, const FrameDescription& description, const LightUniformData& lights, const ShadowPlan& shadows,
                      const Frustum& cameraFrustum);
    void drawMesh(FrameContext& frame, const DrawItem& draw);
    void drawToneMap(FrameContext& frame, const VulkanPipeline& pipeline, const ToneMappingSettings& settings, const BloomComposite& bloomMix);

    void bindMesh(VkCommandBuffer commands, MeshHandle mesh, std::uint32_t& indexCount) const;
    const VulkanTexture& textureOrWhite(TextureHandle handle) const;
    SampledImage sampled(const VulkanTexture& texture) const;
};
