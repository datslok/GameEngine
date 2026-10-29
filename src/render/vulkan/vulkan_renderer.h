#pragma once

#include "core/job_system.h"
#include "math/mat4.h"
#include "render/gpu/bloom.h"
#include "render/gpu/clusters.h"
#include "render/gpu/ddgi_sampling.h"
#include "render/gpu/display_output.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/light_priority.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/shadow_atlas.h"
#include "render/gpu/shadow_cache.h"
#include "render/gpu/shadow_map.h"
#include "render/gpu/transparency.h"
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
#include "render/vulkan/vulkan_atmosphere.h"
#include "render/vulkan/vulkan_eye_adaptation.h"
#include "render/vulkan/vulkan_stars.h"
#include "render/vulkan/vulkan_dlss.h"
#include "render/vulkan/vulkan_fsr.h"
#include "render/vulkan/vulkan_taa.h"
#include "render/vulkan/vulkan_reactive_mask.h"
#include "render/vulkan/vulkan_opaque_picture.h"
#include "render/vulkan/vulkan_bloom.h"
#include "render/vulkan/cloth_pass_stamps.h"
#include "render/vulkan/vulkan_cloth.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_ddgi.h"
#include "render/vulkan/vulkan_debug_lines.h"
#include "render/vulkan/vulkan_depth_range.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_mesh.h"
#include "render/vulkan/vulkan_overlay.h"
#include "render/vulkan/vulkan_pipeline.h"
#include "render/vulkan/vulkan_ray_tracing.h"
#include "render/vulkan/vulkan_mirrored_light.h"
#include "render/vulkan/vulkan_ambient_occlusion.h"
#include "render/vulkan/vulkan_fog.h"
#include "render/vulkan/vulkan_reflections.h"
#include "render/vulkan/vulkan_swapchain.h"
#include "render/vulkan/vulkan_texture.h"
#include "scene/asset_handles.h"
#include "scene/frame_description.h"
#include "scene/indexed_mesh.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct ImDrawData;

// How a pass draws its batches: its pipeline for the plain ones and for cut-outs (the same one where the holes need no
// throwing away), and the cull mode for one-sided batches (double-sided ones cull nothing).
struct BatchPipelines {
    VkPipeline plain = VK_NULL_HANDLE;
    VkPipeline cutOut = VK_NULL_HANDLE;
    VkCullModeFlags cull = VK_CULL_MODE_BACK_BIT;

    // The pipelines also read each mesh's last shape, at vertex binding 1 (the velocity pass).
    bool previousShapes = false;
};

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
    // as setFramesInFlight takes it. jobs runs CPU work on every core (the BRDF table at start-up); it is borrowed, so it
    // must outlive the renderer.
    VulkanRenderer(SDL_Window* window, JobSystem& jobs, bool validation, std::uint32_t framesInFlight);
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    // Draw and present one whole frame. The frame's camera gets the acquired image's aspect ratio (the caller's camera
    // is not changed); the settings are read fresh each frame. ui is the debug UI's finished frame (ImGui::GetDrawData()),
    // drawn over everything, or null. Returns false, drawing nothing, when no image was available (a minimised window).
    bool render(const FrameDescription& frame, const RenderSettings& settings, ImDrawData* ui = nullptr);

    // The same frame drawn off screen at width x height for a fixed display output, and read back (engine/golden.h): the
    // scene image and the tone mapped image as that display would get it. Presents nothing; waits for the GPU. A frame
    // with light probes is drawn probeWarmUpFrames times first, so the probes have gathered their light and bounces.
    CapturedFrame capture(const FrameDescription& frame, const RenderSettings& settings, Uint32 width, Uint32 height,
                          const DisplayOutput& output, int probeWarmUpFrames = 0);

    // The GPU's name as the driver reports it, for keeping golden images per GPU.
    std::string getDeviceName() const;

    // Upload the next mesh or texture. They are numbered in upload order, which must match the AssetManager's handles.
    // Textures are RGBA, four bytes per pixel, colour (sRGB) or data (linear), with their smaller levels when those were
    // built on the CPU (a cut-out's, keeping its coverage). Between frames only.
    // A deforming mesh (scene/deforming_mesh.h) gets a ring of buffers its shape is written into each frame
    // (FrameDescription::deformedMeshes); mesh is then its rest shape.
    void uploadMesh(const IndexedMesh& mesh, bool deforming = false);
    void uploadTexture(Uint32 width, Uint32 height, std::span<const Uint8> pixels, TextureEncoding encoding, std::span<const ImageLevel> mips = {});
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

    /*
    * Cloth simulated on the GPU (render/vulkan/vulkan_cloth.h), for the next render(): each listed cloth's ticks run
    * first thing in its commands, then its mesh is shaped at alpha between its last two ticks (its frame entry
    * DeformedMesh::shapedOnGpu) and its points copied for the CPU. A cloth seen for the first time, or whose points,
    * pins or mesh changed, is uploaded from its CPU state; cloths no longer listed are dropped. Between frames only;
    * the work is used once.
    */
    void setClothWork(std::vector<ClothWork> work, float alpha);

    // Each GPU cloth's points and velocities as a finished frame left them (a few frames late), each returned once.
    std::vector<ClothReadback> takeClothReadbacks();

    /*
    * Whether to time the cloth's passes by kind (RenderStats::cloth): a timestamp after each of the thousand or so a
    * frame, so only while someone reads them (the debug readout). Off by default, and without timestamp support.
    */
    void setClothPassTiming(bool measure);

private:
    SDL_Window* window = nullptr;
    JobSystem& jobs;

    // NVIDIA's Streamline, for DLSS (render/vulkan/vulkan_dlss.h): it must start before the device, which turns on the
    // extensions it asks for, and stop before the device goes (the destructor stops it first).
    StreamlineLibrary streamline;

    // Declared before everything else that is made from it, so it is destroyed after them.
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

        // The cloth ticks this frame's commands ran, reported with its timings (RenderStats::clothTicks).
        std::uint32_t clothTicks = 0;
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
    OwnedHandle<VkSampler> pyramidSampler;     // trilinear, clamped, every level (the reflections' colour pyramid)
    OwnedHandle<VkSampler> nearestLevelsSampler; // exact texels, clamped, every level (the reflection probe's distance pyramid)
    OwnedHandle<VkSampler> shadowSampler;      // compares depths, blending the four nearest results
    OwnedHandle<VkSampler> shadowDepthSampler; // raw depths, exact texels (the soft shadow blocker search)

    // The scene pipelines' descriptors: every texture in one array, and the layout of the frame's scene set. Before the
    // pipelines, which use the layout, so they go first.
    std::unique_ptr<BindlessTextures> bindlessTextures;
    std::unique_ptr<SceneLayout> sceneLayout;

    // The main pass alone (finding the nearest surface itself) or after the depth prepass (shading only it), sampling
    // the light probes or not, writing the reflection surfaces or not, and without the prepass drawing cut-outs (throwing
    // away their holes) or not: every combination, indexed by mainPipelineIndex; on GPUs with ray queries each again
    // tinting its shadowed lights through glass by rays (triangle_rays.frag, in sceneRaysLayout), for frames with glass in
    // the world. And the prepass and the shadow pass, each with its cut-out pipeline (cut_out.frag), which the others need
    // none of.
    std::array<std::unique_ptr<VulkanPipeline>, 32> mainPipelines;
    std::unique_ptr<VulkanPipeline> depthPrepassPipeline;
    std::unique_ptr<VulkanPipeline> depthPrepassCutOutPipeline;
    std::unique_ptr<VulkanPipeline> shadowPipeline;
    std::unique_ptr<VulkanPipeline> shadowCutOutPipeline;
    std::unique_ptr<VulkanPipeline> shadowClearPipeline;
    std::unique_ptr<VulkanPipeline> toneMapPipeline;
    VkFormat screenFormat = VK_FORMAT_UNDEFINED;

    // Clustered shading (render/gpu/clusters.h): the compute pass that lists which lights may reach each cluster, its
    // grid for this frame, and the lists, one bit per light per cluster, sized for the frame like the images. Only the
    // GPU writes and reads them, within a frame, so there is one copy, ordered by barriers rather than one per frame.
    std::unique_ptr<ComputePipeline> clusterLightsPipeline;
    ClusterGrid clusterGrid;
    VulkanBuffer clusterMasks;

    // The main pass draws into these, at the render size (render/gpu/upscaling.h): the lit scene in 16-bit floats, and
    // its depth. The frame's own size is the output's, the swapchain's or a capture's.
    VulkanImage sceneColour;
    VulkanImage depth;
    std::uint32_t frameWidth = 0;
    std::uint32_t frameHeight = 0;
    RenderSize sceneSize;

    // The picture everything after the scene reads (eye adaptation, bloom, tone mapping, captures), at the frame's
    // size: temporal anti-aliasing's result, or the scene image itself while it is off. Set by drawScene.
    VulkanImage* finishedScene = nullptr;

    // Screen-space reflections (render/gpu/screen_reflections.h), and whether last frame's finished picture may be
    // reflected: it was smoothed (so it is not the scene image this frame draws over) and nothing was remade since.
    std::unique_ptr<VulkanReflections> reflections;
    bool previousPictureUsable = false;

    // Lights reflected off mirrors (render/gpu/mirrored_light.h): null without ray queries.
    std::unique_ptr<VulkanMirroredLights> mirroredLights;

    // Ambient occlusion from the depth prepass (render/gpu/ambient_occlusion.h).
    std::unique_ptr<VulkanAmbientOcclusion> ambientOcclusion;

    // Volumetric fog (render/gpu/volumetric_fog.h): its grid, lit after the shadows, and the picture seen through it.
    std::unique_ptr<VulkanFog> fog;

    // The reflection probe round the camera (render/gpu/reflection_probe.h): its cubes, and the capture's pipelines
    // without and with the light probes (probe_capture.frag), each for cut-outs too, indexed by capturePipelineIndex. On GPUs with ray queries they trace the sun's shadow past
    // its cascades (probe_capture_rays.frag), in a layout with the ray tracing set as set 2, which the main pass's glass
    // ray pipelines share.
    std::unique_ptr<VulkanReflectionProbe> reflectionProbe;
    std::array<std::unique_ptr<VulkanPipeline>, 4> capturePipelines;
    OwnedHandle<VkPipelineLayout> sceneRaysLayout;

    // Shadows: every shadow view in one depth atlas, kept between frames (tiles are redrawn only when they change).
    // Counts frames for the memory allocator, which refreshes the driver's memory figures when it changes.
    std::uint32_t memoryFrameNumber = 0;

    VulkanImage shadowAtlas;
    ShadowAtlasLayout shadowLayout;
    ShadowTileMemory shadowMemory;
    LightHistory lightHistory;

    // Fitting the sun's cascades to the screen (cascades.h): the depth measurement read back but not yet used (from a
    // frame or two ago), and where the cascades end now.
    std::unique_ptr<VulkanDepthRange> depthRange;

    // The eye's adaptation (render/gpu/eye_adaptation.h): its passes and the last reading, for the readout. It snaps
    // to its target when it starts afresh (the first frame, or switched on again) and on every frame a capture draws.
    std::unique_ptr<VulkanEyeAdaptation> eyeAdaptation;
    std::optional<GpuEyeAdaptation> lastEyeAdaptation;
    bool eyeAdaptationFresh = true;
    bool capturing = false;

    // Temporal anti-aliasing (render/gpu/taa.h): its images and resolve, the velocity pass's pipeline, the frame's place
    // in the jitter cycle, this frame's camera-relative view-projection without the jitter, and last frame's camera
    // (for reprojection; unknown on the first frame, after it was off and when a capture starts).
    std::unique_ptr<VulkanTaa> taa;

    // The reactive mask (render/gpu/transparency.h): where glass changed the picture, for the anti-aliasing to trust its
    // history less there.
    std::unique_ptr<VulkanReactiveMask> reactiveMask;

    // The picture before the glass, with its blurred levels and distances (render/gpu/transparency.h): refracting glass
    // reads it, and the reactive mask compares the picture after with it.
    std::unique_ptr<VulkanOpaquePicture> opaquePicture;
    std::unique_ptr<VulkanPipeline> motionPipeline;
    std::unique_ptr<VulkanPipeline> motionCutOutPipeline;

    // Glass and blended surfaces (render/gpu/transparency.h): drawn after the opaque scene and its reflections, before
    // anti-aliasing, without and with the light probes. Their layout keeps the scene's sets and adds the reflection
    // probe's light as set 2; their shader tints per channel by dual-source blending where the GPU has it.
    OwnedHandle<VkDescriptorSetLayout> transparentSetLayout;
    OwnedHandle<VkPipelineLayout> transparentLayout;
    std::array<std::unique_ptr<VulkanPipeline>, 2> transparentPipelines;
    std::uint64_t taaFrame = 0;
    Mat4 motionViewProjection = Mat4::identity();

    struct PreviousCamera {
        Mat4 viewProjection = Mat4::identity();
        Vec3 origin{0.0f, 0.0f, 0.0f};
        bool known = false;
    };

    PreviousCamera previousCamera;

    // AMD's FSR 3.1 (render/vulkan/vulkan_fsr.h) and NVIDIA's DLSS (render/vulkan/vulkan_dlss.h), which upscale in place
    // of the resolve when chosen and available; the upscaler this frame uses, and whether each also drew the last frame
    // (else its history is dropped).
    std::unique_ptr<VulkanFsr> fsr;
    std::unique_ptr<VulkanDlss> dlss;
    Upscaler frameUpscaler = Upscaler::Taa;
    bool fsrHistory = false;
    bool dlssHistory = false;

    UpscalerSupport upscalerSupport() const;
    std::optional<ScreenDepthRange> unusedDepthRange;
    ShadowDistanceFit sunShadowFit;

    // Hardware ray tracing (null on a GPU without ray queries): every mesh's acceleration structure and each frame's
    // scene, and the debug view drawn from them (the scene's two sets plus the ray tracing set, and its camera as push
    // constants).
    std::unique_ptr<VulkanRayTracing> rayTracing;
    OwnedHandle<VkPipelineLayout> rayDebugLayout;
    std::unique_ptr<VulkanPipeline> rayDebugPipeline;

    // Global illumination's light probes (render/gpu/ddgi.h), traced with ray queries: null without them.
    std::unique_ptr<VulkanDdgi> ddgi;

    std::unique_ptr<VulkanBloom> bloom;

    // The sky and the air the light crosses, when the frame has an atmosphere.
    std::unique_ptr<VulkanAtmosphere> atmosphere;

    // The stars behind the sky, when the frame has an atmosphere and a starry sky.
    std::unique_ptr<VulkanStars> starPass;
    std::unique_ptr<VulkanDebugLines> debugLines;
    std::unique_ptr<VulkanOverlay> overlay;
    bool debugUiStarted = false;

    // Cloth on the GPU: the passes (made with the first cloth), each cloth by its key with the frames in flight whose
    // readback it is waiting for, the next frame's work, and the readbacks collected and not yet taken.
    struct GpuCloth {
        std::unique_ptr<VulkanCloth> cloth;
        MeshHandle mesh;
        std::array<bool, maxFramesInFlight> readbackPending{};
    };
    std::unique_ptr<VulkanClothPipelines> clothPipelines;

    // Every field body's samples any cloth has met, shared by them all.
    std::unique_ptr<VulkanClothFields> clothFields;
    std::map<std::uint64_t, GpuCloth> gpuCloths;
    std::vector<ClothWork> clothWork;
    float clothAlpha = 1.0f;
    std::vector<ClothReadback> clothReadbacks;

    // The cloth passes' timestamps, made the first time they are asked for, and the last frame's not yet reported;
    // likewise the self-collision pair counts read back with the points.
    bool clothPassTiming = false;
    std::unique_ptr<ClothPassStamps> clothStamps;
    ClothPassTimings unreportedClothTimings;
    ClothSelfPairCounts unreportedClothSelf;

    // This frame's slot for each mesh a cloth shapes on the GPU, by mesh index (set by showShapes).
    std::map<std::uint32_t, std::uint32_t> gpuShapeSlots;

    // GPU copies, indexed exactly like MeshHandle and TextureHandle.
    std::vector<std::unique_ptr<VulkanMesh>> meshes;
    std::vector<std::unique_ptr<VulkanTexture>> textures;
    std::unique_ptr<VulkanTexture> whiteTexture;
    std::unique_ptr<VulkanTexture> brdfTable;

    // Bound in place of the light probes' atlases while there are none (one black texel, never read then).
    std::unique_ptr<VulkanTexture> noProbesTexture;

    // Bound in place of the light probes' states while there are none (one probe's worth of zeros, never read then).
    VulkanBuffer noProbeStates;
    VulkanBuffer noProbeCascades;

    // This frame's camera in the atmosphere's frame: its distance from the planet's centre, km (0 without an atmosphere).
    float cameraAtmosphereRadius = 0.0f;

    // This frame's camera in world space (culling, debug lines) and measured from the camera (the main pass).
    Mat4 viewProjection = Mat4::identity();
    Mat4 relativeViewProjection = Mat4::identity();
    Vec3 cameraOrigin{0.0f, 0.0f, 0.0f};

    RenderStats stats;

    // The last frame's GPU timings read back and not yet handed to stats (each frame's are reported once), and the
    // cloth ticks that frame ran.
    GpuTimings unreportedGpuTimings;
    std::uint32_t unreportedClothTicks = 0;

    void checkFormats() const;
    void createFrames();
    void createSamplers();
    void createSceneLayout();
    void createPipelines();
    // The main pass's pipelines in this layout, tinting shadowed lights through glass by rays or not.
    void createMainPipelines(bool glassRays, VkPipelineLayout layout);
    void createScreenPipelines(VkFormat format);
    std::unique_ptr<VulkanPipeline> createToneMapPipeline(VkFormat format);
    void createShadowAtlas(const ShadowAtlasSize& size);
    void ensureShadowAtlas(const ShadowAtlasSize& size);
    void createFixedTextures();
    void ensureFrameTextures(std::uint32_t width, std::uint32_t height, const RenderSettings& settings);

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

    // probes: the frame's probe cascades when this frame traces and samples them, else empty; hitLights: the lights their
    // rays' hits may need (empty without probes); motionDraws: the objects that moved, for the velocity pass.
    // captureDraws: each reflection probe face's draws, following the velocity pass's in the draw list (none without a capture).
    // mirrorPairs: the lights and shiny surfaces the mirrored light search runs over (none when off); the main pass
    // reads the mirrored light pass's images when there are any. occlusion: the main pass reads ambient occlusion, the
    // blurred image when occlusionBlurred. glassShadows: shadowed lights are tinted by the glass in the world's bounds.
    // frameFog: the fog's record (off without fog).
    VkDescriptorSet writeScene(FrameContext& frame, const FrameDescription& description, const GpuLights& lights, const ShadowPlan& shadows,
                               const BatchedDraws& mainDraws, const ShadowDraws& shadowDraws, const BatchedDraws& motionDraws,
                               std::span<const BatchedDraws> captureDraws, const SortedTransparentDraws& transparent, const RenderSettings& settings,
                               std::span<const ProbeCascade> probes,
                               const ProbeHitLights& hitLights, std::span<const MirrorPair> mirrorPairs, bool occlusion, bool occlusionBlurred,
                               bool glassShadows, const GpuFog& frameFog);

    // Whether this frame traces (and samples) light probes: ray tracing, global illumination on, and ProbeCascades.
    bool tracesProbes(const FrameDescription& description, const RenderSettings& settings) const;
    void buildClusterLists(FrameContext& frame, VkDescriptorSet sceneSet);
    ShadowDraws prepareShadowDraws(const FrameDescription& description, const std::vector<ShadowTile>& tiles, std::uint32_t firstEntry);
    void drawShadows(FrameContext& frame, const ShadowDraws& shadowDraws, VkDescriptorSet sceneSet);
    void drawDepthPrepass(FrameContext& frame, const BatchedDraws& mainDraws, VkDescriptorSet sceneSet);
    // sampleProbes: the frame's light probes light the surfaces (the pipelines that sample them).
    // sky: the camera's rays when the frame has an atmosphere, whose sky is drawn over the background (none: the background alone),
    // with stars, when the frame has them, over it.
    // reflectionSurfaces: the main pass also writes the reflection surfaces, and the sky and stars are drawn after it,
    // in a pass of their own (their pipelines draw into the scene image alone).
    // glassRaySet: the frame's ray tracing set while shadowed lights are tinted through glass (the glass ray pipelines),
    // else none.
    void drawMainPass(FrameContext& frame, const BatchedDraws& mainDraws, VkDescriptorSet sceneSet, bool afterPrepass, bool keepDepth,
                      bool sampleProbes, bool reflectionSurfaces, VkDescriptorSet glassRaySet, const Vec3& background, const GpuRayDebugView* sky,
                      const StarrySky* stars);
    void updateSunShadowFit(const ShadowSettings& settings, const Camera& camera);
    void createRayTracing();
    GpuAtmosphere packFrameAtmosphere(const FrameDescription& description, const GpuLights& lights) const;
    void drawProbeBalls(FrameContext& frame, VkDescriptorSet sceneSet, int cascade);
    void drawRayDebugView(FrameContext& frame, VkDescriptorSet sceneSet, const Camera& camera, RayDebugView view);
    void drawMotion(FrameContext& frame, const BatchedDraws& motionDraws, VkDescriptorSet sceneSet, const GpuMotionView& view);
    // The transparent draws, far to near, blended over the scene image (its depth tested, not written), sampling the
    // light probes or not, reading the reflection probe when this frame captured it, refracting glass reading the picture
    // before the glass as refraction says (opaquePicture, kept first).
    void drawTransparent(FrameContext& frame, const SortedTransparentDraws& transparent, VkDescriptorSet sceneSet, bool sampleProbes, bool readsProbe,
                         const TransparentRefraction& refraction);

    // The reflection probe's six faces from the camera, each with its draws, then its sky (none: the background alone)
    // over them, and the probe's distances and prefiltered light made from them.
    void drawProbeCapture(FrameContext& frame, std::span<const BatchedDraws> captureDraws, VkDescriptorSet sceneSet, bool sampleProbes,
                          float farPlane, const Vec3& background, bool sky);
    // Draw frames of the description before a capture, the first as given, the rest as repeated (the same frame without
    // its probe events, which happen once).
    void drawFramesBeforeCapture(const FrameDescription& description, const FrameDescription& repeated, const RenderSettings& settings, int frames);
    std::size_t drawBatches(VkCommandBuffer commands, const std::vector<DrawBatch>& batches, const BatchPipelines& pipelines) const;
    void drawToneMap(FrameContext& frame, const VulkanPipeline& pipeline, const ToneMappingSettings& settings, const BloomComposite& bloomMix);

    // Each deforming mesh shows the frame's shape for it, or holds its last; after the frame's fence, before anything is
    // recorded. Throws std::invalid_argument for a deformed mesh that was not uploaded as deforming.
    void showShapes(const FrameDescription& description);

    // The frame's cloth work (setClothWork), recorded right after showShapes.
    void runCloths(VkCommandBuffer commands);

    // After the frame's fence: the readbacks its last use of each cloth asked for.
    void collectClothReadbacks();

    void bindMesh(VkCommandBuffer commands, MeshHandle mesh, std::uint32_t& indexCount, bool previousShape = false) const;
    void checkTexturesUploaded(const Material& material) const;
    SampledImage sampled(const VulkanTexture& texture) const;
};
