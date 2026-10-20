#pragma once

#include "math/vec3.h"
#include "render/gpu/ddgi.h"
#include "render/gpu/ddgi_sampling.h"
#include "render/gpu/probe_relocation.h"
#include "scene/frame_description.h"
#include "scene/probe_volume.h"

#include <cstdint>
#include <span>
#include <vector>

/*
* Cascades of light probes (docs/ILLUMINATION.md, "Large worlds"): nested grids round a point (the camera), each twice
* the spacing of the last, so twice as far across for the same probes. Bounce light is smooth and a far surface covers
* few pixels, so the detail needed falls with distance, like a texture's mipmaps: four cascades cost four grids and
* reach eight times as far as one.
*
* Each cascade is a window of probes on a lattice fixed in the world (multiples of its spacing from the world's
* origin), the window's middle the lattice point nearest the centre. When the centre crosses into another cell, the
* window moves a cell: the plane of probes that falls out behind is reused for the plane coming in at the front, as a
* circular buffer (ProbeScroll), so every other probe keeps its slot and its light, and only the new plane starts.
*
* A point takes the densest cascade's light where that one reaches, the next where the densest fades out, and so on,
* the flat ambient beyond the coarsest. Each cascade's weight is centred on the centre itself, not on its window, so it
* moves smoothly with the camera and nothing pops when a plane jumps; and it ends far enough inside the window that no
* point it lights reads a cell touching an outer plane, which may have just come in.
*/

inline constexpr int maxProbeCascades = DDGI_MAX_CASCADES;
inline constexpr float probeCascadeFadeCells = static_cast<float>(DDGI_CASCADE_FADE);
inline constexpr float probeCascadeEdgeCells = static_cast<float>(DDGI_CASCADE_EDGE);

// How many cascades of how many probes, and the densest's spacing.
struct ProbeCascadeLayout {
    ProbeCounts counts;
    float spacing = 0.5f;
    int cascades = 1;
};

/*
* The layout the settings ask for, kept within what the atlases hold: at least 2 probes along each axis, 1 to
* maxProbeCascades cascades, as many cascades as fit in maxProbesPerVolume probes and the atlases' largest side, and a
* spacing of at least a centimetre.
*/
ProbeCascadeLayout probeCascadeLayout(const GlobalIlluminationSettings& settings);

// Cascade number cascade's spacing: the densest's, doubled cascade times.
float probeCascadeSpacing(const ProbeCascadeLayout& layout, int cascade);

/*
* The first grid place, in cells from the world's origin, of a window of count probes spacing apart round centre: the
* lattice point nearest centre is the window's middle probe (number (count - 1) / 2, rounded down).
*/
int probeWindowStart(float centre, float spacing, int count);

/*
* How far from the centre a window of count probes spacing apart lights points, in metres (its weight falls to 0 there):
* its nearest outer plane is at least (count - 1) / 2 (rounded down) less half a cell away, since the centre lies up to
* half a cell from the middle probe; the weight ends probeCascadeEdgeCells and the self-shadow bias's reach short of
* that. A point's biased point then lies more than a cell inside every outer plane, so its cell never touches one.
*/
float probeCascadeReach(int count, float spacing, float biasReach);

/*
* One cascade: its window of probes (volume, scrolled so each lattice point keeps its slot), the window's first grid
* place counted from the world's origin (start), where its probes begin among all the cascades' (firstProbe: the
* atlases, the states, the restarts), and where it lights points: at full weight out to weightReach - weightFade from
* weightCentre along each axis, falling to nothing at weightReach.
*/
struct ProbeCascade {
    ProbeVolume volume;
    ProbeGridCoord start;
    int firstProbe = 0;
    Vec3 weightCentre{0.0f, 0.0f, 0.0f};
    Vec3 weightReach{0.0f, 0.0f, 0.0f};
    Vec3 weightFade{0.0f, 0.0f, 0.0f};
};

// The window of counts probes spacing apart round centre, weighted round centre (probeCascadeReach, fading over
// probeCascadeFadeCells cells); its probes numbered from firstProbe.
ProbeCascade probeWindow(const Vec3& centre, float spacing, const ProbeCounts& counts, float selfShadowBias, int firstProbe);

// Every cascade of the layout round centre, densest first, their probes numbered one cascade after another.
std::vector<ProbeCascade> probeCascadesAround(const Vec3& centre, const ProbeCascadeLayout& layout, float selfShadowBias);

/*
* A volume that stays put, as a cascade: full weight out to half a cell beyond its outer probes, fading to nothing over
* the next half (DDGI_VOLUME_MARGIN, DDGI_VOLUME_FADE), as the probes' first single volume did; for tests that place
* probes by hand.
*/
ProbeCascade fixedProbeCascade(const ProbeVolume& volume, int firstProbe = 0);

// How much a cascade lights a point, 0..1: the product over the axes of its fade (ProbeCascade).
float probeCascadeWeight(const ProbeCascade& cascade, const Vec3& point);

// The grid place, counted from the world's origin, of the probe kept in slot slot.
ProbeGridCoord probeLatticeCoord(const ProbeCascade& cascade, int slot);

/*
* The probes (numbered as the cascades number them, from firstProbe) whose slot holds another lattice point after than
* before: the planes a move brought in. Every probe when the two differ in size or spacing.
*/
std::vector<int> scrolledProbes(const ProbeCascade& before, const ProbeCascade& after);

/*
* All the cascades' light at a point (sampleProbes on each), from cascade fromCascade on, densest first: each adds its
* light times its weight times what the denser ones left (1 - their weights so far), until that reaches 1. A cascade
* with no traced probe round the point adds nothing and leaves its share to the coarser ones; whatever is left over is
* lookup.ambient for still light and nothing for fast light. lookup's probe numbers count among all cascades
* (firstProbe + slot).
*/
ProbeLights sampleProbeCascades(std::span<const ProbeCascade> cascades, int fromCascade, float selfShadowBias, const Vec3& point, const Vec3& normal,
                                const Vec3& lookupNormal, const Vec3& reflected, const Vec3& toViewer, const ProbeLookup& lookup);

/*
* A cascade as the shaders read it (must match ProbeCascade in probe_cascades.glsl, std430): positions measured from the
* camera, as the main pass measures everything.
*/
struct GpuProbeCascade {
    float origin[4];        // xyz the window's first probe, w the longest distance its probes record (probeMaxDistance)
    float spacing[4];       // xyz the spacing between probes, w unused
    std::int32_t scroll[4]; // xyz the storage's scroll, w the cascade's first probe
    float weightCentre[4];  // xyz where its weight is centred, w unused
    float weightReach[4];   // xyz how far it lights from the centre, w unused
    float weightFade[4];    // xyz how far its weight takes to fall to nothing, w unused
};

static_assert(sizeof(GpuProbeCascade) == 96, "GpuProbeCascade must match ProbeCascade in probe_cascades.glsl");

std::vector<GpuProbeCascade> packProbeCascades(std::span<const ProbeCascade> cascades, const Vec3& cameraOrigin);

/*
* Every cascade's probe states (buildProbeStates on each, with its own spacing), one after another: relocations holds
* every probe's record (or none yet: every probe vigilant), movers what wakes sleeping probes.
*/
std::vector<GpuProbeState> buildCascadeStates(std::span<const ProbeCascade> cascades, std::span<const GpuProbeRelocation> relocations,
                                              std::span<const BoundingSphere> movers, const GlobalIlluminationSettings& settings);

/*
* A probe the relocation pass is asked to place, and the lattice point it stood at then: its record is kept only if the
* probe is still there when the record comes back, a frame or two later.
*/
struct ProbeRelocationRequest {
    int probe = 0;
    ProbeGridCoord place;
};

// Every probe of the cascades, each at its lattice point now.
std::vector<ProbeRelocationRequest> relocationRequests(std::span<const ProbeCascade> cascades, std::span<const int> probes);

/*
* A record for a probe whose relocation has not come back yet (a plane a scroll brought in): off, never traced or read,
* until its record arrives (classifyProbe).
*/
GpuProbeRelocation pendingProbeRelocation();
bool probeRelocationPending(const GpuProbeRelocation& relocation);

/*
* Keep a relocation's records (found, one per request, in order) for the probes still at the lattice point they were
* asked for at; the rest stay as they are (pending, if a scroll forgot them). records holds every probe of the cascades.
*/
void keepProbeRelocations(std::vector<GpuProbeRelocation>& records, std::span<const ProbeRelocationRequest> requests,
                          std::span<const GpuProbeRelocation> found, std::span<const ProbeCascade> cascades);

/*
* Which of the probes that start this frame (restartedProbes) are seeded from the coarser cascades' light: all but the
* coarsest cascade's (which has none coarser), at most maxProbeSeeds, in order, each given the next seed slot.
*/
std::vector<ProbeSeed> chooseProbeSeeds(std::span<const int> restarted, std::span<const ProbeCascade> cascades);
