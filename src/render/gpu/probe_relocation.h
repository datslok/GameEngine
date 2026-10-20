#pragma once

#include "math/vec3.h"
#include "render/gpu/ddgi.h"
#include "render/gpu/ddgi_shared.h"
#include "scene/frame_description.h"
#include "scene/probe_volume.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <vector>

/*
* Moving the light probes off the level and choosing which ones to trace (the 2021 DDGI paper's relocation and probe
* states; docs/ILLUMINATION.md). A grid ignores the level's shape: some probes land inside walls, where they see only
* back faces and lend darkness to the surfaces around them, and some a hair from a surface, whose distances then say
* little. At start-up each probe traces a fixed spiral of rays against the static level and moves (at most
* probeOffsetLimit of a spacing per axis, so it stays in its own cell) out through the nearest back face, or away from
* a front face that is too close; a probe that cannot get out is off. Then each probe's state: vigilant when a static
* surface lies within its cells (the 8 cells it is a corner of, one spacing each way: exactly the surfaces that read
* it), sleeping when none does (nothing reads it, so it is not traced), awake when something that moves comes near a
* sleeping one. The pure functions are mirrored by assets/shaders/probe_relocation.glsl.
*/

inline constexpr int probeRelocationRays = DDGI_RELOCATION_RAYS;
inline constexpr int probeRelocationIterations = DDGI_RELOCATION_ITERATIONS;
inline constexpr float insideBackFaceShare = static_cast<float>(DDGI_INSIDE_BACK_FACE_SHARE);
inline constexpr float probeOffsetLimit = static_cast<float>(DDGI_OFFSET_LIMIT);
inline constexpr float probeEscapeMargin = static_cast<float>(DDGI_ESCAPE_MARGIN);
inline constexpr float probeMinFrontFaceShare = static_cast<float>(DDGI_MIN_FRONT_FACE_SHARE);
inline constexpr float probeFrontFaceNudge = static_cast<float>(DDGI_FRONT_FACE_NUDGE);
inline constexpr float sameSurfaceCosine = static_cast<float>(DDGI_SAME_SURFACE_COSINE);
inline constexpr float probeRelocationMinDistance = static_cast<float>(DDGI_RELOCATION_MIN_DISTANCE);

// The distance a ray that met nothing reports (the trace's whole reach).
inline constexpr float probeRayMiss = static_cast<float>(DDGI_RAY_REACH);

enum class ProbeState : std::int32_t {
    Off = DDGI_PROBE_OFF,
    Sleeping = DDGI_PROBE_SLEEPING,
    Awake = DDGI_PROBE_AWAKE,
    Vigilant = DDGI_PROBE_VIGILANT
};

// Awake and vigilant probes are traced, updated and read; off and sleeping ones are not.
bool probeTraced(ProbeState state);

/*
* What a probe's rays found, as relocation and classification need it: how many met back and front faces, the nearest
* back face, the nearest and farthest front faces (distance and direction), and how far the nearest front face hit lies
* beyond the probe's own cells (one spacing each way round its grid point), worst axis first, in metres: 0 or less is
* inside them.
*/
struct ProbeRayTally {
    int rays = 0;
    int backFaces = 0;
    int frontFaces = 0;
    float closestBackFace = std::numeric_limits<float>::max();
    Vec3 closestBackFaceDirection{0.0f, 0.0f, 0.0f};
    float closestFrontFace = std::numeric_limits<float>::max();
    Vec3 closestFrontFaceDirection{0.0f, 0.0f, 0.0f};
    float farthestFrontFace = 0.0f;
    Vec3 farthestFrontFaceDirection{0.0f, 0.0f, 0.0f};
    float surfaceBeyondCells = std::numeric_limits<float>::max();
};

/*
* Count one ray: its direction, and its distance as the trace reports it (negative for a back face, probeRayMiss or
* more for nothing). start: where the probe is, measured from its grid point.
*/
void tallyProbeRay(ProbeRayTally& tally, const Vec3& direction, float signedDistance, const Vec3& start, const Vec3& spacing);

/*
* The largest scale s for which offset + s step stays within limit on every axis, less probeEscapeMargin along the
* step: each axis the step moves along allows (limit - offset) / step towards the side it moves to, and the tightest
* wins. Negative when the offset is already past the limit on an axis the step moves further along.
*/
float probeEscapeScale(const Vec3& offset, const Vec3& step, const Vec3& limit);

// One move: the probe's new offset from its grid point, and whether it is stuck inside something it cannot leave.
struct ProbeMove {
    Vec3 offset{0.0f, 0.0f, 0.0f};
    bool stuck = false;
};

/*
* One relocation step from the rays traced at offset:
* - inside something (more than insideBackFaceShare of the rays met back faces): through the nearest back face, scaled
*   as far as the limit allows (probeEscapeScale); if that does not reach the face (scale 1 or less) it is stuck;
* - a front face nearer than probeMinFrontFaceShare of the smallest spacing: up to probeFrontFaceNudge of it towards
*   the farthest front face, unless that lies the same way (sameSurfaceCosine), clamped to the limit;
* - otherwise where it is.
*/
ProbeMove relocateProbeStep(const ProbeRayTally& tally, const Vec3& offset, const Vec3& spacing);

/*
* What the relocation pass leaves for each probe (must match ProbeRelocation in probe_relocation.glsl): its offset from
* its grid point, whether it is off (stuck, or still inside something: 1, else 0), and how far its nearest front face
* lies beyond its cells (ProbeRayTally::surfaceBeyondCells, the largest float when it sees none), which the CPU compares
* with the bias's reach each frame, so changing the bias needs no new pass. A probe that sees no static surface at all
* is not off, unlike the paper's: it sleeps, so something that moves can still wake it (a level with nothing marked
* static then lights its movers as before).
*/
struct GpuProbeRelocation {
    float offset[3] = {0.0f, 0.0f, 0.0f};
    float off = 0.0f;
    float surfaceBeyondCells = 0.0f;
    float unused[3] = {0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(GpuProbeRelocation) == 32, "GpuProbeRelocation must match ProbeRelocation in probe_relocation.glsl");

// The record for the final rays, traced at offset (stuck: a step found it could not get out).
GpuProbeRelocation finishProbeRelocation(const ProbeRayTally& tally, const Vec3& offset, bool stuck);

// One ray from origin along a unit direction against the static level: its distance as tallyProbeRay takes it, hits
// nearer than probeRelocationMinDistance skipped as the GPU's relocation rays skip them.
using ProbeRayTrace = std::function<float(const Vec3& origin, const Vec3& direction)>;

/*
* The whole relocation of one probe, as ddgi_relocate.comp does it: up to iterations steps, each tracing the fixed
* spiral (probeRelocationRays) from where the probe is, stopping once it stays put or is stuck, then the rays once more
* from where it ended for its record. 0 iterations only classifies it where the grid puts it.
*/
GpuProbeRelocation relocateProbe(const Vec3& gridPosition, const Vec3& spacing, int iterations, const ProbeRayTrace& trace);

/*
* A relocated probe's state while nothing moves near it: off as relocation found it, vigilant when its nearest front
* face lies within its cells grown by biasReach (probeBiasReach: a surface's biased point can reach that far, and the
* cell the biased point falls in is what picks the probes), sleeping otherwise.
*/
ProbeState classifyProbe(const GpuProbeRelocation& relocation, float biasReach);

// Each probe as every probe pass reads it (must match ProbeState in probe_relocation.glsl): its offset and its state.
struct GpuProbeState {
    float offset[3] = {0.0f, 0.0f, 0.0f};
    std::int32_t state = static_cast<std::int32_t>(ProbeState::Vigilant);
};

static_assert(sizeof(GpuProbeState) == 16, "GpuProbeState must match ProbeState in probe_relocation.glsl");

/*
* This frame's state of every probe: with no relocation yet (an empty span, or one of another size) every probe
* vigilant where the grid puts it, exactly as before relocation existed; otherwise each probe's offset (none while
* settings.probeRelocation is off) and class (classifyProbe), and a sleeping probe wakes while a mover's bounds come
* within its cells grown by the bias's reach and settings.wakeCells more cells (so it has settled by the time the mover
* arrives). Movers without bounds (radius below 0) wake nothing. settings.probeStates off: every probe vigilant.
*/
std::vector<GpuProbeState> buildProbeStates(const ProbeVolume& volume, std::span<const GpuProbeRelocation> relocations,
                                            std::span<const BoundingSphere> movers, const GlobalIlluminationSettings& settings);

// The bounds of every draw that is not static and has bounds: what wakes sleeping probes.
std::vector<BoundingSphere> movingBounds(std::span<const DrawItem> draws);

/*
* The probes that hold light from another time or place: traced now but not before (woken, or newly vigilant), or
* moved. They restart outright (advanceProbeRestarts). None when the sizes differ: a new volume starts fresh anyway.
*/
std::vector<int> restartedProbes(std::span<const GpuProbeState> before, std::span<const GpuProbeState> after);

// How many probes are in each state this frame (traced: awake and vigilant), and how many relocation moved.
struct ProbeStateCounts {
    int probes = 0;
    int traced = 0;
    int awake = 0;
    int sleeping = 0;
    int off = 0;
    int moved = 0;
};

ProbeStateCounts countProbeStates(std::span<const GpuProbeState> states);
