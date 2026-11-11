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
* black when what blocks it is lit: each ray that meets a surface also brings back the light that surface sends, its
* albedo rho times the light falling on it (the sky it sees there, and the bounce there, read from the grid itself), per
* unit of the open sky's light. Those values, per colour channel, are kept as the same two bands (SkyBounce), so a
* surface gets f + the bounce's own a + 2/3 b . n: the light from all around as a share of the flat ambient, which the
* shaders multiply by it live. Kept per unit, it needs no re-bake when the sky's light changes with the time of day.
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

    // The least light from all around any surface gets, as a share of the open sky's: a room seen only through a sliver
    // of sky reads dim, not black (0.025, what the earlier guess's floor gave). Read live; changing it bakes nothing.
    float ambientFloor = 0.025f;

    // The densest cascade's spacing (metres); each further one's is twice the last's.
    float spacing = 0.5f;

    // How many cascades (1 to maxSkyVisibilityCascades); past the last, the sky is taken as open, as today.
    int cascades = 2;

    // Rays a point (on a Fibonacci sphere) and how far each looks (metres): a ray running that far counts as open. 256:
    // at 64, neighbouring points' measurements differed by up to half (9 rays through a window against 6), and the floor
    // blending between them showed soft blobs the grid's size; the bake's frame budget keeps the frame rate as it was.
    int rays = 256;
    float rayLength = 30.0f;

    // The rays a point gets in a refill (the first fill, a new level or shape, a long jump), when every waiting point is
    // baked at once: at 256 a refill froze the frame about 2.6 s, at 64 about 0.65. The points are then baked again at
    // the full rays within the frame budget, the blobs 64 rays leave gone a second or two later. Not part of the grid's
    // shape: changing it bakes nothing again.
    int quickRays = 64;

    // How much memory may keep baked points by their place in the world after they leave the grid (megabytes, 0 off),
    // so coming back to a place finds them instead of baking them again (render/gpu/sky_visibility_bake.h,
    // SkyCellStore). Used only as places are visited; the points used longest ago go first. Not part of the grid's shape.
    float storeMegabytes = 256.0f;

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

// Throws std::invalid_argument for an ambient floor outside 0..1, a spacing or ray length not above 0, a negative bake
// time, lookup bias or store, cascades outside 1..maxSkyVisibilityCascades, or rays or quick rays below 1.
void validateSkyVisibility(const SkyVisibilitySettings& settings);

// Whether two settings bake the same grid: everything but the ambient floor, the lookup bias, the budget, the quick rays,
// the store and the view.
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

/*
* The light bounced back to a point from the surfaces round it, per unit of the open sky's light: for each colour
* channel (red, green, blue) the two bands of what its rays brought back (open the mean, lean towards where it came
* from), read as skySeenFacing and skySeenAlong read the sky's. None by default.
*/
using SkyBounce = std::array<SkyVisibility, 3>;
inline const SkyBounce noSkyBounce{SkyVisibility{0.0f, Vec3{0.0f, 0.0f, 0.0f}}, SkyVisibility{0.0f, Vec3{0.0f, 0.0f, 0.0f}},
                                       SkyVisibility{0.0f, Vec3{0.0f, 0.0f, 0.0f}}};

/*
* The light from all around a surface with this unit normal gets, per colour channel, as a share of the open sky's: the
* sky it sees plus each channel's bounce facing it (each kept within 0..1), at least floor.
*/
Vec3 skyLightFacing(const SkyVisibility& visibility, const SkyBounce& bounce, const Vec3& normal, float floor);

// The same along one unit direction (the reflection's stand-in): the sky and the bounce seen along it.
Vec3 skyLightAlong(const SkyVisibility& visibility, const SkyBounce& bounce, const Vec3& direction, float floor);

// The same for air, which scatters light from every way alike: the sky's open share and each channel's mean.
Vec3 skyLightInAir(const SkyVisibility& visibility, const SkyBounce& bounce, float floor);

enum class SkyCellState : std::uint8_t {
    Unknown = SKY_VISIBILITY_UNKNOWN,
    Baked = SKY_VISIBILITY_BAKED,
    Buried = SKY_VISIBILITY_BURIED
};

// The six axis directions a point's reach is measured along, in this order: +x, -x, +y, -y, +z, -z.
inline constexpr int skyReachDirections = 6;
Vec3 skyReachDirection(int direction);

/*
* One grid point: its bands, the bounce light round it, how far it sees along each axis direction as a share of its
* cascade's spacing (at most 1), and its state. Not baked yet by default.
*/
struct SkyCell {
    SkyVisibility visibility;
    SkyBounce bounce = noSkyBounce;
    std::array<float, skyReachDirections> reach{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    SkyCellState state = SkyCellState::Unknown;

    // The bake's own note, not packed for the shaders: the share of its rays' front hits whose light was guessed
    // because the grid round them was not baked yet, which baking it again later would improve.
    float unsureShare = 0.0f;
};

/*
* Whether a grid point sees a point fromCorner away from it, both within a cell spacing apart: along each axis, the
* offset's share of the spacing is within the point's reach that way. Walls in levels mostly stand along the axes, so
* one blocking the way usually cuts that axis's reach short. A rounding step's slack, so a point at the reach still sees.
*/
bool skyCornerSees(const std::array<float, skyReachDirections>& reach, const Vec3& fromCorner, float spacing);

/*
* A grid point as the shaders read it (sky_visibility.glsl, unpackSkyCell), three texels of four words: a and b's x as
* two halves, b's y and z as two halves, the reaches along +x, -x, +y, -y in 8 bits each, and those along +z and -z and
* the state in the last; then each bounce channel's a and b's x, b's y and z, as halves, red, green and blue (two words
* each); the last two words unused.
*/
struct GpuSkyCell {
    std::uint32_t words[12];
};

static_assert(sizeof(GpuSkyCell) == 48, "GpuSkyCell must match sky_visibility.glsl's packed cell");

// The texels each grid point takes in the shaders' image, along x.
inline constexpr int skyCellTexels = SKY_CELL_TEXELS;

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
* its share to the next. What is left past the last is the open sky, with no bounce. cells holds every cascade's points
* by number. Also how much of it the cascades knew (known, 0..1): what the bake falls back on a guess for where little is;
* and how much a cascade reached but had no point to read (missing: not baked yet, or all buried or behind walls), as
* against what lies past the grid, which waiting will not change.
*/
struct SkySample {
    SkyVisibility visibility;
    SkyBounce bounce = noSkyBounce;
    float known = 0.0f;
    float missing = 0.0f;
};

SkySample sampleSkyVisibility(std::span<const ProbeCascade> cascades, std::span<const SkyCell> cells, float lookupBias, const Vec3& point,
                              const Vec3& normal, const Vec3& toViewer);

/*
* The settings the shaders read (sky_visibility.glsl), in the frame's uniform block: x how many cascades the frame's
* records hold (0 when the grid is not read), y the ambient floor, z the lookup bias, w 1 for the debug view. All zero
* when it is not read.
*/
struct GpuSkyVisibilityOptions {
    float values[4];
};

GpuSkyVisibilityOptions packSkyVisibilityOptions(const SkyVisibilitySettings& settings, int cascadesRead);
