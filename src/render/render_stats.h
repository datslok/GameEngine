#pragma once

#include "render/gpu/gpu_timing.h"
#include "render/gpu/probe_relocation.h"
#include "render/gpu/video_memory.h"

#include <cstddef>

// What the last frame drew and what it skipped, counting each mesh once per view (each shadow tile is a view).
// Shadow views also skip meshes that do not cast shadows.
struct RenderStats {
    std::size_t drawn = 0;
    std::size_t culled = 0;
    std::size_t shadowDrawn = 0;
    std::size_t shadowCulled = 0;

    // Draw calls those took: one per mesh per view, each drawing every instance of that mesh at once. The camera's
    // include the depth prepass's, which draws the same meshes again.
    std::size_t drawCalls = 0;
    std::size_t shadowDrawCalls = 0;

    // Shadow views drawn this frame, kept from an earlier frame because nothing in them changed (cached), and point
    // light cube faces left out because they see nothing on screen. Shadow drawn/culled above count only drawn views.
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

    // The light probes' states this frame (render/gpu/probe_relocation.h); all zero without probes.
    ProbeStateCounts probes;

    // How long each pass took on the GPU, from timestamps. They can only be read once the GPU has finished a frame, so
    // these are an earlier frame's (as many back as there are frames in flight); not valid until one has been measured.
    GpuTimings gpu;

    // The video memory in use and the driver's budget, read as this frame began (render/gpu/video_memory.h).
    VideoMemory videoMemory;
};