#pragma once

#include "core/settings_visitor.h"
#include "math/vec3.h"
#include "render/gpu/probe_cascades.h"
#include "render/gpu/sky_visibility_shared.h"
#include "scene/probe_volume.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

/*
* Sky visibility: how much of the sky each point round the camera can see, so the flat ambient light (the sky's light
* on an open floor, the same for every surface) dims indoors without light probes or ray tracing.
*
* At each point of a grid, rays on the CPU against the static level (render/gpu/sky_visibility_bake.h) say which ways
* are open. That is a function on the sphere, V(w), 1 where a ray escapes and 0 where it is blocked, kept as its first
* two spherical harmonic bands: V(w) ~ a + b . w, with a = (1 / 4 pi) integral of V (the share of the sphere open) and
* b = (3 / 4 pi) integral of V w (pointing to the open side). A surface with normal n receives the sky in proportion to
*
*   f(n) = (1 / pi) integral of V(w) max(0, n . w) dw = a + (2 / 3) b . n,
*
* exactly 1 for a floor under an open sky, 1/2 for a wall in an open field, 0 in a closed box. A blocked way is not
* black when what blocks it is lit: it brings back the bounce share beta of the sky's light times how lit the blockers
* are, guessed from the point's own open share (g = clamp(2a, 0, 1): under an open sky a point sees half the sphere and
* its ground sees all of it). The share a surface gets is f + beta g (1 - f): unchanged outdoors (g = 1), and a closed
* room, whose walls see no sky either, gets nothing back, so it goes dark.
*
* The points sit in cascades round the camera, as the light probes' do (render/gpu/probe_cascades.h, whose windows,
* scrolling and weights this reuses): each a window on a lattice fixed in the world, a circular buffer, so a move
* bakes only the plane it brings in. A point is read as the probes are: the eight round it weighted by nearness, by
* which side of the surface they lie (probeBackFaceWeight) and by whether a wall stands between (each point's reach
* along the six axis directions, skyCornerSees); points inside walls and points not baked yet are left out.
*/

inline constexpr ProbeCounts skyVisibilityCounts{SKY_VISIBILITY_ACROSS, SKY_VISIBILITY_HIGH, SKY_VISIBILITY_ACROSS};
inline constexpr int maxSkyVisibilityCascades = SKY_VISIBILITY_MAX_CASCADES;
inline constexpr float skyVisibilityBuriedShare = static_cast<float>(SKY_VISIBILITY_BURIED_SHARE);
inline constexpr int skyVisibilityReachSteps = SKY_VISIBILITY_REACH_STEPS;

struct SkyVisibilitySettings {
    // On every preset but Low; read only without light probes, which know the sky's reach themselves.
    bool enabled = true;

    // The light a blocked way still brings when what blocks it stands under open sky, as a share of the open sky's:
    // what blocks it reflects some (an average surface's albedo). Read live; changing it bakes nothing.
    float bounceShare = 0.25f;

    // The least a blocker counts as lit, as a share of the open sky's light (the guess is twice the point's open share):
    // a room seen only through a sliver of sky reads dim, not black. Read live.
    float blockersLitFloor = 0.1f;

    // The densest cascade's spacing (metres); each further one's is twice the last's.
    float spacing = 0.5f;

    // How many cascades (1 to maxSkyVisibilityCascades); past the last, the sky is taken as open, as today.
    int cascades = 2;

    // Rays a point (on a Fibonacci sphere) and how far each looks (metres): a ray running that far counts as open.
    int rays = 64;
    float rayLength = 30.0f;

    // How far a surface's lookup is moved off it, as the probes' self-shadow bias (a share of their formula's offset).
    float lookupBias = 0.3f;

    // The CPU time a frame may spend baking the points a move brought in (milliseconds, on every core). The first fill,
    // and a refill after the level or the grid's shape changed, is baked whole at once.
    float bakeMilliseconds = 1.0f;

    // A debug view: the light from all around shown as grey, the share of the sky each surface sees.
    bool debugView = false;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(SkyVisibilitySettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a bounce share or blockers' floor outside 0..1, a spacing or ray length not above 0, a negative bake
// time or lookup bias, cascades outside 1..maxSkyVisibilityCascades, or rays below 1.
void validateSkyVisibility(const SkyVisibilitySettings& settings);

// Whether two settings bake the same grid: everything but the bounce share and floor, the lookup bias, the budget and the view.
bool sameSkyVisibilityGrid(const SkyVisibilitySettings& a, const SkyVisibilitySettings& b);

// The open sphere's two harmonic bands: a (the share open) and b (towards the open side). The default is all open.
struct SkyVisibility {
    float open = 1.0f;
    Vec3 lean{0.0f, 0.0f, 0.0f};
};

// Ray number ray of rays from a point, spread evenly over the sphere (ddgi.h, sphericalFibonacci).
Vec3 skyVisibilityRay(int ray, int rays);

/*
* The bands from rays that sample the sphere evenly: a the mean of the openings (each 0 or 1), b three times the mean of
* opening times direction (the integrals above, each ray standing for 4 pi / N of the sphere). The directions are taken
* about their own mean, which a finite spiral leaves a little off zero, so a sphere open (or closed) all round has no lean.
*/
SkyVisibility skyVisibilityFromRays(std::span<const Vec3> directions, std::span<const float> openings);

// The sky a surface with this unit normal sees, f = clamp(a + 2/3 b . n, 0, 1), and along one unit direction (the
// reflection's stand-in), clamp(a + b . d, 0, 1).
float skySeenFacing(const SkyVisibility& visibility, const Vec3& normal);
float skySeenAlong(const SkyVisibility& visibility, const Vec3& direction);

// How lit what blocks the sky is guessed to be, as a share of the open sky's light: g = clamp(2a, 0, 1), at least floor.
float skyBlockersLit(const SkyVisibility& visibility, float floor);

// The share of the open sky's light a surface with this unit normal gets: f + beta g (1 - f).
float skyVisibilityFacing(const SkyVisibility& visibility, const Vec3& normal, float bounceShare, float blockersFloor = 0.0f);

// The share along one unit direction (the reflection's stand-in): the same with the sky seen along it.
float skyVisibilityAlong(const SkyVisibility& visibility, const Vec3& direction, float bounceShare, float blockersFloor = 0.0f);

// The share for air, which scatters light from every way alike: the same with f = clamp(a, 0, 1).
float skyVisibilityInAir(const SkyVisibility& visibility, float bounceShare, float blockersFloor = 0.0f);

enum class SkyCellState : std::uint8_t {
    Unknown = SKY_VISIBILITY_UNKNOWN,
    Baked = SKY_VISIBILITY_BAKED,
    Buried = SKY_VISIBILITY_BURIED
};

// The six axis directions a point's reach is measured along, in this order: +x, -x, +y, -y, +z, -z.
inline constexpr int skyReachDirections = 6;
Vec3 skyReachDirection(int direction);

/*
* One grid point: its bands, how far it sees along each axis direction as a share of its cascade's spacing (at most 1),
* and its state. Not baked yet by default.
*/
struct SkyCell {
    SkyVisibility visibility;
    std::array<float, skyReachDirections> reach{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    SkyCellState state = SkyCellState::Unknown;
};

/*
* Whether a grid point sees a point fromCorner away from it, both within a cell spacing apart: along each axis, the
* offset's share of the spacing is within the point's reach that way. Walls in levels mostly stand along the axes, so
* one blocking the way usually cuts that axis's reach short. A rounding step's slack, so a point at the reach still sees.
*/
bool skyCornerSees(const std::array<float, skyReachDirections>& reach, const Vec3& fromCorner, float spacing);

/*
* A grid point as the shaders read it (sky_visibility.glsl, unpackSkyCell): a and b's x as two halves, b's y and z as
* two halves, the reaches along +x, -x, +y, -y in 8 bits each, and those along +z and -z and the state in the last.
*/
struct GpuSkyCell {
    std::uint32_t words[4];
};

static_assert(sizeof(GpuSkyCell) == 16, "GpuSkyCell must match sky_visibility.glsl's packed cell");

GpuSkyCell packSkyCell(const SkyCell& cell);
SkyCell unpackSkyCell(const GpuSkyCell& packed);

// Cascade number cascade's spacing: the densest's doubled cascade times.
float skyVisibilitySpacing(const SkyVisibilitySettings& settings, int cascade);

/*
* The cascades round centre, densest first, each a window of skyVisibilityCounts points (probeWindow) on a lattice
* offset probeLatticeOffsetShare of the densest spacing from the world's origin (so no point lies on a floor at a whole
* or half metre), their points numbered one cascade after another.
*/
std::vector<ProbeCascade> skyVisibilityCascades(const Vec3& centre, const SkyVisibilitySettings& settings);

// Every cascade's points together.
int skyVisibilityCellCount(const SkyVisibilitySettings& settings);

/*
* The sky's reach at a surface point (normal its unit normal, toViewer the unit way to the eye; a zero normal for a point
* in the air): each cascade, densest first, adds its blend times its weight round the camera times what the denser ones
* left, as the probes' cascades do (sampleProbeCascades). Within a cascade the eight points round the lookup point
* (moved off the surface as the probes' self-shadow bias moves it) are blended by nearness, by side (probeBackFaceWeight,
* crushed as the probes' weights are) and only where skyCornerSees; a cascade with no such point adds nothing and leaves
* its share to the next. What is left past the last is the open sky. cells holds every cascade's points by number.
*/
SkyVisibility sampleSkyVisibility(std::span<const ProbeCascade> cascades, std::span<const SkyCell> cells, float lookupBias, const Vec3& point,
                                  const Vec3& normal, const Vec3& toViewer);

/*
* The settings the shaders read (sky_visibility.glsl), in the frame's uniform block: x how many cascades the frame's
* records hold (0 when the grid is not read), y the bounce share, z the lookup bias, w 1 for the debug view; then x the
* blockers' floor. All zero when it is not read.
*/
struct GpuSkyVisibilityOptions {
    float values[4];
    float more[4];
};

GpuSkyVisibilityOptions packSkyVisibilityOptions(const SkyVisibilitySettings& settings, int cascadesRead);
