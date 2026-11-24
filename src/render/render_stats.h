#pragma once

#include "render/cpu_timing.h"
#include "render/gpu/cloth_timing.h"
#include "render/gpu/eye_adaptation.h"
#include "render/gpu/gpu_timing.h"
#include "render/gpu/probe_relocation.h"
#include "render/gpu/twig_drawing_shared.h"
#include "render/gpu/upscaling.h"
#include "render/gpu/video_memory.h"
#include "render/gpu/virtual_shadows.h"
#include "scene/mesh_level.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

// What the last frame drew and what it skipped, counting each mesh once per view (each shadow tile is a view).
// Shadow views also skip meshes that do not cast shadows.
struct RenderStats {
    std::size_t drawn = 0;
    std::size_t culled = 0;

    // How many of the camera's draws drew each level of detail (scene/level_of_detail.h): the meshes themselves first.
    std::array<std::size_t, meshLevelSlots> drawnByLevel{};

    // The triangles the camera's draws drew (the cut-outs' among them), and the reflection probe's faces drawn this frame.
    std::size_t drawnTriangles = 0;
    std::size_t drawnCutOutTriangles = 0;
    std::size_t probeTriangles = 0;

    // The camera's mesh clusters (render/gpu/mesh_cluster_culling.h), while culled: tested, kept (both lists), and the
    // kept ones' triangles; none tested while off. With occlusion: those the late list drew, and those neither drew.
    std::size_t meshClustersTested = 0;
    std::size_t meshClustersKept = 0;
    std::size_t meshClusterTriangles = 0;
    bool meshClusterOcclusion = false;
    std::size_t meshClustersLate = 0;
    std::size_t meshClustersHidden = 0;

    // The camera's twigs (render/gpu/twig_records.h's TwigTally), while drawn: tested, kept, kept moving (posed, or kept
    // whole as their part sways unbound), and the kept ones' twigs and triangles at each level; none tested while not drawn.
    std::size_t twigsTested = 0;
    std::size_t twigsKept = 0;
    std::size_t twigsSwaying = 0;
    std::array<std::size_t, TWIG_MOST_LEVELS> twigsByLevel{};
    std::array<std::size_t, TWIG_MOST_LEVELS> twigTrianglesByLevel{};
    // Their far levels (stage 3e-iii): bricks handing over to a far node, far nodes drawn (those merged from bricks apart),
    // their levels' leaf clusters and triangles (before the cluster passes' culling).
    std::size_t twigFarBricks = 0;
    std::size_t twigFarNodes = 0;
    std::size_t twigFarMerged = 0;
    std::size_t twigFarClusters = 0;
    std::size_t twigFarTriangles = 0;
    // The far leaves the mesh shaders tested against a pyramid and hid (stage 3e-v-b): the main pass's, the late
    // prepass's; none tested without far leaf occlusion.
    std::size_t twigFarLeavesTested = 0;
    std::size_t twigFarLeavesHidden = 0;
    std::size_t twigFarLeavesLateTested = 0;
    std::size_t twigFarLeavesLateHidden = 0;

    // The bones pass (render/gpu/bone_poses.h), while it runs: trees posed, those left still for want of room, the
    // bones asked for and the pool's room; none posed while it does not run.
    std::size_t boneTrees = 0;
    std::size_t boneTreesStill = 0;
    std::size_t bonesAsked = 0;
    std::size_t bonePool = 0;

    // The sun's virtual shadow maps' pages (render/gpu/virtual_shadows.h), while they are on: asked for, kept, given
    // room, taken from cached ones, failed for want of room, freed as stale, left free; none while off.
    std::optional<VirtualShadowCounts> virtualShadowPages;

    // And its casters, while its clusters are culled (mesh shaders): draws taken, clusters tested and kept, triangles.
    std::optional<VirtualShadowCasters> virtualShadowCasters;

    std::size_t shadowDrawn = 0;
    std::size_t shadowCulled = 0;

    // Draw calls those took: one per mesh per view, each drawing every instance of that mesh at once. The camera's
    // include the depth prepass's, which draws the same meshes again.
    std::size_t drawCalls = 0;
    std::size_t shadowDrawCalls = 0;

    // Shadow views drawn this frame, kept from an earlier frame because nothing in them changed (cached), and point
    // light cube faces left out because they see nothing on screen. Shadow drawn/culled above count only drawn views.
    // The cells holding still draws and their slots (scene/still_draws.h), and the bytes the frame copied into GPU memory
    // kept between frames (render/vulkan/vulkan_lasting_buffer.h): standing still, only the moving draws' records.
    std::size_t stillCells = 0;
    std::size_t stillSlots = 0;
    std::size_t lastingCopiedBytes = 0;

    std::size_t shadowTilesDrawn = 0;
    std::size_t shadowTilesCached = 0;
    std::size_t shadowTilesSkipped = 0;

    // How far along the view the sun's cascades reach, in metres: the shadow distance, or nearer when fitted to the
    // surfaces on screen. 0 without a shadowed sun.
    float sunShadowDistance = 0.0f;

    // Point lights in the frame, how many could reach what the camera sees, and how many got a seat in the shader and a shadow.
    std::size_t pointLights = 0;
    std::size_t pointLightsInView = 0;
    std::size_t pointLightsLit = 0;
    std::size_t pointLightsShadowed = 0;

    // Image lights this frame (render/gpu/mirrored_light.h, chooseImageLights): lights seen in flat mirrors.
    std::size_t imageLights = 0;

    // The light probes' states this frame (render/gpu/probe_relocation.h); all zero without probes.
    ProbeStateCounts probes;

    // How long each pass took on the GPU, from timestamps. They can only be read once the GPU has finished a frame, so
    // these are an earlier frame's (as many back as there are frames in flight); not valid until one has been measured.
    GpuTimings gpu;

    // The GPU cloth ticks the frame gpu timed ran (every cloth runs the same ticks), as late as gpu: with gpu's cloth
    // time, what a tick costs the GPU (engine/gpu_cloth.h, gpuClothTickSeconds).
    std::uint32_t clothTicks = 0;

    // The cloth's GPU time by kind of pass, as late as gpu; valid only while measured (VulkanRenderer::setClothPassTiming)
    // and some cloth ran on the GPU.
    ClothPassTimings cloth;

    // The video memory in use and the driver's budget, read as this frame began (render/gpu/video_memory.h).
    VideoMemory videoMemory;

    // The eye's adapted exposure as an earlier frame left it (as many back as there are frames in flight), while it
    // adapts (render/gpu/eye_adaptation.h).
    std::optional<GpuEyeAdaptation> eyeAdaptation;

    // The upscaler that drew the frame (TAA when FSR was asked for but is unavailable, Off for none) and the render
    // scale it drew at.
    Upscaler upscaler = Upscaler::Off;
    float renderScale = 1.0f;

    // The width of the image the frame was presented in, in the pixels screen lines are drawn in (the debug
    // readout wraps to it); 0 before a frame was drawn.
    std::uint32_t outputWidth = 0;

    // The height of the image the frame was presented in (after upscaling), which the camera's levels of detail are
    // chosen for; 0 before a frame was drawn.
    std::uint32_t outputHeight = 0;

    // The renderer's CPU time by stage this frame (render/cpu_timing.h).
    CpuStageTimings cpu;
};