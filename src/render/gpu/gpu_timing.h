#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

/*
* The renderer's passes in the order a frame records them, each timed on the GPU. The cloth runs first (its ticks and
* the shape pass writing its meshes; render/gpu/cloth_timing.h breaks it down by pass), then the debug UI is drawn into
* its own image; the ray tracing scene is built only while something traces rays (the light probes, the ray debug view);
* tone mapping, the debug lines and the overlay share one pass over the screen image but are timed apart.
*/
enum class GpuPass {
    Cloth,
    DebugUi,
    RayScene,
    Atmosphere,
    Probes,
    Clusters,
    Shadows,
    Fog,          // the fog's grid lit and integrated (render/gpu/volumetric_fog.h)
    DepthPrepass,
    AmbientOcclusion, // ambient occlusion from the prepass's depth, and its blur (render/gpu/ambient_occlusion.h)
    MirroredLights, // light reflected off mirrors onto what the prepass drew (render/gpu/mirrored_light.h)
    Main,
    ProbeBalls,
    RayDebug,
    Motion,
    DepthRange,
    ReflectionProbe, // the reflection probe's capture: its six faces, their mips, distances and prefiltered levels
    Reflections,  // reflections: the depth and colour pyramids and the reflection pass
    FogApply,     // the picture before the glass seen through the fog
    Transparent,  // glass and blended surfaces, sorted, over the picture with its reflections (render/gpu/transparency.h)
    AntiAliasing, // TAA, FSR or DLSS, whichever smooths the frame (with the camera motion pass before FSR and DLSS)
    EyeAdaptation,
    Bloom,
    ToneMap,
    DebugLines,
    Overlay,
    Count,
};

inline constexpr std::size_t gpuPassCount = static_cast<std::size_t>(GpuPass::Count);

// One timestamp before the first pass and one after each, so consecutive stamps bound every pass.
inline constexpr std::uint32_t gpuTimestampCount = static_cast<std::uint32_t>(gpuPassCount) + 1;

// Short upper-case names for the readout (the stroke font has capitals only).
const char* gpuPassName(GpuPass pass);

// How long each pass of one frame took on the GPU. Not valid until a frame has been measured, or without timestamp support.
struct GpuTimings {
    bool valid = false;
    double totalSeconds = 0.0;
    std::array<double, gpuPassCount> passSeconds{};

    double seconds(GpuPass pass) const {
        return passSeconds[static_cast<std::size_t>(pass)];
    }
};

// Ticks from one stamp to a later one, on a counter whose low validBits bits count (1 to 64) and then roll over to 0.
std::uint64_t timestampTicksBetween(std::uint64_t earlier, std::uint64_t later, std::uint32_t validBits);

// A frame's gpuTimestampCount stamps turned into seconds per pass, with the length of a tick (the device's
// timestampPeriod). validBits 0 means the queue writes no timestamps: the result is not valid.
GpuTimings gpuTimingsFromTicks(std::span<const std::uint64_t> ticks, double nanosecondsPerTick, std::uint32_t validBits);
