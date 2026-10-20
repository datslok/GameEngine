#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/ddgi.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/probe_relocation.h"
#include "scene/probe_volume.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

/*
* Reading global illumination's light probes back at a surface (docs/ILLUMINATION.md, "Shading a point from the
* probes"): the 8 probes at the corners of the grid cell around the point are blended, each weighted by how near it is
* (trilinear), whether it lies in front of the surface (a soft wrap) and whether it can see the point at all (Chebyshev's
* bound on its stored distances, as in variance shadow maps), which keeps light from leaking through walls. The blend
* gives the irradiance arriving at the surface, in the units of the ambient light, so it takes the flat ambient's place
* as diffuse light. Also what a probe ray's hit adds from point and spot lights (the brightest few, each by a shadow ray),
* since probes rays reach far beyond what the shadow atlas covers. The functions are mirrored by
* assets/shaders/ddgi_sampling.glsl and checked against it by the GPU tests.
*/

inline constexpr float biasNormalShare = static_cast<float>(DDGI_BIAS_NORMAL_SHARE);
inline constexpr float biasViewShare = static_cast<float>(DDGI_BIAS_VIEW_SHARE);
inline constexpr float biasSpacingShare = static_cast<float>(DDGI_BIAS_SPACING_SHARE);
inline constexpr float probeWrapFloor = static_cast<float>(DDGI_WRAP_FLOOR);
inline constexpr float probeVisibilityPower = static_cast<float>(DDGI_VISIBILITY_POWER);
inline constexpr float minProbeVisibility = static_cast<float>(DDGI_MIN_VISIBILITY);
inline constexpr float smallestProbeWeight = static_cast<float>(DDGI_SMALLEST_WEIGHT);
inline constexpr float probeCrushThreshold = static_cast<float>(DDGI_CRUSH_THRESHOLD);
inline constexpr float minTrilinearFactor = static_cast<float>(DDGI_MIN_TRILINEAR);
inline constexpr float probeVolumeMarginCells = static_cast<float>(DDGI_VOLUME_MARGIN);
inline constexpr float probeVolumeFadeCells = static_cast<float>(DDGI_VOLUME_FADE);
inline constexpr int maxLightsPerHit = DDGI_MAX_HIT_LIGHTS;

/*
* The self-shadow bias [2021]: the point tested for visibility, moved off the surface mostly towards the viewer (who by
* definition sees it from open space) and a little along the normal, by biasSpacingShare of the smallest spacing times
* the setting. A point on a wall sits exactly where the probes' rays stop, the worst place for the visibility test.
*/
Vec3 probeSelfShadowBias(const Vec3& normal, const Vec3& toViewer, const Vec3& spacing, float selfShadowBias);

// The longest the self-shadow bias can be (its normal and view parts both at full length): how far a surface's biased
// point, which picks the probes that light it, can lie from the surface.
float probeBiasReach(const Vec3& spacing, float selfShadowBias);

// The grid cell around a point: its first corner (clamped into the grid) and how far across the cell the point lies
// along each axis (0..1, clamped).
struct ProbeCell {
    ProbeGridCoord base;
    Vec3 alpha{0.0f, 0.0f, 0.0f};
};

ProbeCell probeCell(const ProbeVolume& volume, const Vec3& point);

// Corner 0..7 of the cell (bit 0 along x, bit 1 along y, bit 2 along z), clamped to the grid: past the last probe on an
// axis both corners are that probe.
ProbeGridCoord probeCellCorner(const ProbeCell& cell, const ProbeCounts& counts, int corner);

// The corner's trilinear weight: per axis alpha or 1 - alpha, each at least minTrilinearFactor.
float probeTrilinearWeight(const Vec3& alpha, int corner);

/*
* How much a probe counts for its side of the surface, from the offset to it (unbiased): ((cos + 1) / 2)^2 plus the wrap
* floor, a soft curve rather than a cut at the surface's plane, which could reject every probe that sees a small detail.
* A probe exactly at the point counts fully.
*/
float probeBackFaceWeight(const Vec3& normal, const Vec3& toProbe);

/*
* Whether a probe can see a point at this distance, from the mean and mean square of its distances in that direction:
* 1 up to the mean; beyond it Chebyshev's one-sided bound on the chance that a ray travels that far,
* variance / (variance + (distance - mean)^2), raised to probeVisibilityPower, and never below minProbeVisibility.
*/
float probeVisibility(float distance, const Vec2& moments);

// A weight kept at least smallestProbeWeight, and below probeCrushThreshold shrunk to weight^3 / threshold^2.
float crushProbeWeight(float weight);

/*
* One corner probe's whole weight: back-face weight times visibility, crushed, times the trilinear weight (after the
* crush, so nearness alone is never crushed). toProbe from the surface point, distance from the biased point.
*/
float probeCornerWeight(const Vec3& normal, const Vec3& toProbe, float distance, const Vec2& moments, float trilinear);

/*
* How much a cascade lights a point along each axis, multiplied (render/gpu/probe_cascades.h): 1 out to reach - fade
* from its centre, falling linearly to nothing at reach.
*/
float probeCascadeWeight(const Vec3& centre, const Vec3& reach, const Vec3& fade, const Vec3& point);

/*
* The blend of the corners: the square root of each stored irradiance, weighted, then the sum divided by the summed
* weights and squared [2019, 2021]. Averaging square roots gives one bright probe less pull. This is interpolation
* between probes, each holding a settled value, not averaging noisy samples over time, so it does not darken the way the
* fifth-root update did (render/gpu/ddgi.h, blendProbeValue).
*/
struct ProbeBlend {
    Vec3 sum{0.0f, 0.0f, 0.0f};
    float weight = 0.0f;
};

void addProbeSample(ProbeBlend& blend, const Vec3& irradiance, float weight);
Vec3 finishProbeBlend(const ProbeBlend& blend);

/*
* What sampling reads from the atlases, as the GPU reads it filtered: a probe's stored irradiance from still and from
* fast lights (ddgi.h, GlobalIlluminationSettings::fastIrradianceTimeConstant) and its distances' mean and mean square,
* each in a direction; and each probe's state and offset (none: every probe traced where the grid puts it), and the
* flat ambient light a point with no traced probe round it gets.
*/
struct ProbeLookup {
    std::function<Vec3(int probe, const Vec3& direction)> irradiance;
    std::function<Vec2(int probe, const Vec3& direction)> moments;
    std::function<Vec3(int probe, const Vec3& direction)> fastIrradiance;
    std::function<GpuProbeState(int probe)> state = nullptr;
    Vec3 ambient{0.0f, 0.0f, 0.0f};
};

/*
* What the probes give a surface: the irradiance for its diffuse part, and the same blend read in the mirror direction
* for its specular part, a stand-in for reflections until they exist (a reflection as blurred as the roughest surface's,
* [2021]'s "reflections from probes"), so a glossy floor indoors reflects the room's light rather than the open sky.
*/
struct ProbeLight {
    Vec3 diffuse{0.0f, 0.0f, 0.0f};
    Vec3 specular{0.0f, 0.0f, 0.0f};
};

/*
* The light of both atlases, each blended on its own with the same corner weights: the screen adds them, and a probe
* ray's hit passes each on into its own atlas, so a fast light's later bounces keep its short memory.
*/
struct ProbeLights {
    ProbeLight still;
    ProbeLight fast;
};

/*
* The probes' light at a surface point with this normal seen from toViewer: the whole of sampling, as
* scene_probes.glsl's sampleProbes does it. normal moves the test point and weighs the probes; lookupNormal is the
* direction the irradiance is read in (the normal-mapped normal on screen), reflected the mirror direction. Only traced
* probes count (probe_relocation.h), each where relocation moved it; with none round the point, the flat ambient.
* lookup's probe numbers are the volume's slots (probeSlot) from firstProbe.
*/
ProbeLights sampleProbes(const ProbeVolume& volume, float selfShadowBias, const Vec3& point, const Vec3& normal, const Vec3& lookupNormal,
                         const Vec3& reflected, const Vec3& toViewer, const ProbeLookup& lookup, int firstProbe = 0);

// The same, or nothing when no probe round the point is traced (sampleProbes then gives the flat ambient).
std::optional<ProbeLights> sampleTracedProbes(const ProbeVolume& volume, float selfShadowBias, const Vec3& point, const Vec3& normal,
                                              const Vec3& lookupNormal, const Vec3& reflected, const Vec3& toViewer, const ProbeLookup& lookup,
                                              int firstProbe = 0);

/*
* The light a point or spot light gives a matte surface at a probe ray's hit, before its shadow ray: the irradiance on the
* surface (radiance times falloff, times the cone for a spot, times the cosine), 0 out of range, outside the cone or from
* behind. offsetToLight runs from the surface to the light.
*/
Vec3 pointLightAtHit(const Vec3& offsetToLight, float range, const Vec3& radiance, float sourceRadius, const Vec3& normal);
Vec3 spotLightAtHit(const Vec3& offsetToLight, float range, const Vec3& radiance, float sourceRadius, const Vec3& beamDirection, float cosOuter,
                    float cosInner, const Vec3& normal);

// How bright a light looks, for ranking: its luminance (Rec. 709 weights).
float hitLightStrength(const Vec3& irradiance);

/*
* The brightest lights met so far at one hit, brightest first: a light joins if there is room or it is brighter than the
* dimmest kept, which then drops out. A light giving nothing never joins, and ties keep the earlier light, so the result
* does not depend on rounding.
*/
struct BrightestLights {
    int lights[maxLightsPerHit]{};
    float strengths[maxLightsPerHit]{};
    int count = 0;
};

void keepBrightestLight(BrightestLights& kept, int capacity, int light, float strength);

/*
* The frame's point and spot lights a probe ray's hit may need: those whose range reaches the probes' box grown by the
* longest distance the probes record (probeMaxDistance), as numbers into the frame's light buffers, the point lights'
* first, each marked fast or not (GpuLights::fastPoints). Lights beyond that still light the screen; only their light on
* far surfaces the probes see is left out.
*/
struct GpuProbeHitLight {
    std::int32_t light = 0;
    std::int32_t fast = 0; // 1 for a fast light
};

static_assert(sizeof(GpuProbeHitLight) == 8, "GpuProbeHitLight must match ddgi_trace.comp's ProbeHitLights");

struct ProbeHitLights {
    std::vector<GpuProbeHitLight> lights;
    int points = 0;
    int spots = 0;
    int fast = 0; // how many of them are fast
};

// keepFastApart false (GlobalIlluminationSettings::fastLights off): every light is listed as still, its bounce kept with
// the rest.
ProbeHitLights probeHitLights(const GpuLights& lights, const Vec3& cameraOrigin, const ProbeVolume& volume, bool keepFastApart = true);

/*
* What the frame's shaders need to sample the probes and light their rays' hits, into the frame's uniform block: every
* cascade's probes together (for the atlases' grid), how many cascades (where each is and its size, in the cascades'
* buffer: probe_cascades.h), and the sampling's settings.
*/
struct ProbeSampling {
    int probes = 1;
    int cascades = 1;
    float selfShadowBias = 0.3f;
    float bounceEnergy = 0.85f;
    int lightsPerHit = 4;
    int hitPoints = 0;
    int hitSpots = 0;
};

// Fill frameData's probe fields. Whether pixels sample the probes is the main pass pipeline's choice
// (DDGI_SAMPLE_PROBES_CONSTANT), not a field.
void packProbeSampling(GpuFrameData& frameData, const ProbeSampling& sampling);
