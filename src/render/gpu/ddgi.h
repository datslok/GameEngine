#pragma once

#include "core/settings_visitor.h"
#include "math/quat.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/ddgi_shared.h"
#include "scene/probe_volume.h"

#include <cstdint>

/*
* Global illumination from light probes: DDGI ("Dynamic Diffuse Global Illumination", Majercik et al. 2019, in the form
* of its 2021 production follow-up; research notes and formulas in docs/ILLUMINATION.md). A grid of probes (a
* ProbeVolume) each traces a few rays a frame, and blends what they bring back into two small images of the sphere of
* directions: the light arriving from around each direction (irradiance, in the units of the ambient light: a probe in a
* uniform sky drawn at B stores B), and the mean and mean square distance to the nearest surface (for telling later
* whether a probe can see a point). This is the API-free part: the octahedral images and their borders, the rays'
* directions, the update's weights and blending, and where each probe lives in the atlases. The functions are mirrored
* by assets/shaders/ddgi.glsl and checked against it by the GPU tests.
*/

inline constexpr int probeIrradianceTexels = DDGI_IRRADIANCE_TEXELS;
inline constexpr int probeDistanceTexels = DDGI_DISTANCE_TEXELS;
inline constexpr int minRaysPerProbe = DDGI_MIN_RAYS;
inline constexpr int maxRaysPerProbe = DDGI_MAX_RAYS;
inline constexpr float probeDistanceSharpness = static_cast<float>(DDGI_DISTANCE_SHARPNESS);
inline constexpr float backFaceDistanceShare = static_cast<float>(DDGI_BACK_FACE_DISTANCE_SHARE);
inline constexpr float irradiancePower = static_cast<float>(DDGI_IRRADIANCE_POWER);
inline constexpr float irradianceChangeThreshold = static_cast<float>(DDGI_CHANGE_THRESHOLD);
inline constexpr float irradianceChangeHysteresisDrop = static_cast<float>(DDGI_CHANGE_HYSTERESIS_DROP);
inline constexpr float irradianceResetThreshold = static_cast<float>(DDGI_RESET_THRESHOLD);
inline constexpr float minProbeTexelWeight = static_cast<float>(DDGI_MIN_TEXEL_WEIGHT);
inline constexpr float maxDistanceDiagonals = static_cast<float>(DDGI_MAX_DISTANCE_DIAGONALS);
inline constexpr int maxProbeAtlasSide = DDGI_MAX_ATLAS_SIDE;

// The most probes one volume may hold: one workgroup per probe, and every GPU runs at least 65,535 groups in a row.
inline constexpr int maxProbesPerVolume = 65535;

/*
* Everything about global illumination that a game, the debug UI or a player may tune. Ray tracing only: without ray
* queries there are no probes yet (a software tracer comes later), and nothing here applies.
*/
struct GlobalIlluminationSettings {
    // Trace and update the probes of the frame's first probe volume.
    bool enabled = true;

    // Rays each probe traces per frame. More converge faster and flicker less; the time constants below average over
    // rays per second of history, so a slower GPU can trace fewer with longer time constants.
    int raysPerProbe = 64;

    // How long the stored light takes to follow a change: after this many seconds, 1/e (37%) of a step change is left,
    // at any frame rate. Shorter reacts faster and flickers more. Distances are kept steadier, since flickering
    // visibility is worse than slightly late visibility [2021].
    float irradianceTimeConstant = 0.5f;
    float distanceTimeConstant = 1.0f;

    // How far the point tested for visibility moves off the surface, in units of 0.75 of the smallest spacing [2021].
    // Too little leaves blotches where surfaces meet the probes' distances; too much lets light leak round corners.
    // Raise it when tracing fewer rays (noisier distances).
    float selfShadowBias = 0.3f;

    // The share of last frame's probe light that a probe ray's hit passes on (each frame adds one more bounce). 1 is
    // physically right, since albedo below 1 already makes the bounces die out; a little less hides faint leaks, which
    // repeated bounces would otherwise build up and the eye sees in dark rooms (the 2019 code's 0.85).
    float bounceEnergy = 0.85f;

    // The point and spot lights each probe ray's hit adds, the brightest there, each checked by a shadow ray.
    int lightsPerHit = 4;

    // A debug view: each probe drawn as a small ball showing the light it stores, in the direction of the ball's surface.
    bool showProbes = false;
    float probeBallRadius = 0.1f;
};

void visit(GlobalIlluminationSettings& settings, SettingsVisitor& visitor);

// A texel's place in an image, counted from the top-left texel.
struct TexelCoord {
    int x = 0;
    int y = 0;
};

/*
* The octahedral mapping (Cigolle et al. 2014): a unit direction projected onto the octahedron |x| + |y| + |z| = 1,
* seen from above, with the lower half folded outwards over the four corners, so the whole sphere covers the square
* -1..1 on each axis with texels of nearly equal solid angle. The edges are folds: (1, v) and (1, -v) are the same
* direction, and all four corners are straight down.
*/
Vec2 octahedralEncode(const Vec3& unitDirection);
Vec3 octahedralDecode(const Vec2& square);

// The direction at the centre of an inner texel (0..side - 1 on each axis, without the border) of a probe's square.
Vec3 probeTexelDirection(TexelCoord texel, int side);

/*
* Each probe's square carries a one-texel border, so filtering across its edge reads the right neighbours. In the
* bordered square (side + 2 texels across, the inner texels at 1..side), the texel a border texel copies: an edge texel
* copies the inner texel next to it mirrored about the edge's middle (the fold), a corner the inner texel in the
* opposite corner. Inner texels return themselves.
*/
TexelCoord probeBorderSource(TexelCoord bordered, int side);

// The bordered square's border texels one by one, number 0..4 * side + 3: the top row, the bottom row, the left column,
// the right column (each without corners), then the four corners. Each update group copies them in this order.
int probeBorderTexelCount(int side);
TexelCoord probeBorderTexel(int number, int side);

/*
* Point index of count spread evenly over the sphere: a spherical Fibonacci spiral (Marques et al. 2013), z falling in
* equal steps from top to bottom while the angle around z turns by the golden angle each step, so no two points line up.
*/
Vec3 sphericalFibonacci(int index, int count);

/*
* A rotation drawn uniformly from all rotations (Shoemake 1992), the same for the same frame number: every probe's rays
* are turned by it, so a fixed pattern's gaps move every frame and the samples add up to the whole sphere over time.
*/
Quat randomProbeRotation(std::uint64_t frameNumber);

// The rotation as the shaders take it: its matrix's rows.
struct ProbeRotationRows {
    Vec3 rows[3];
};

ProbeRotationRows probeRotationRows(const Quat& rotation);

// Ray ray of a probe's count this frame: the Fibonacci point turned by the frame's rotation.
Vec3 probeRayDirection(int ray, int count, const ProbeRotationRows& rotation);

/*
* The update's weights for one ray seen from one texel: irradiance weights rays by the cosine between the two (Lambert's
* law), so dividing by the summed weights gives the cosine-weighted mean light, which in the engine's units is the
* irradiance; distances use a much narrower lobe (the cosine to the power probeDistanceSharpness).
*/
float irradianceRayWeight(const Vec3& texelDirection, const Vec3& rayDirection);
float distanceRayWeight(const Vec3& texelDirection, const Vec3& rayDirection);

/*
* The distance a ray contributes to the distance images. The trace records a back-face hit as a negative distance, which
* becomes backFaceDistanceShare of its length, so the probe treats what lies behind as close and hidden; every distance
* is capped at maxDistance, which a miss (recorded as the ray's whole reach) becomes.
*/
float distanceForUpdate(float rayDistance, float maxDistance);

// The longest distance a probe's distances record: maxDistanceDiagonals of its cell's diagonal.
float probeMaxDistance(const Vec3& spacing);

// Irradiance as stored (its irradiancePower-th root, which like a logarithm compresses bright values) and back.
Vec3 encodeIrradiance(const Vec3& irradiance);
Vec3 decodeIrradiance(const Vec3& encoded);

/*
* Keeping a share h of the old value each frame relaxes exponentially, like a capacitor charging, with time constant
* tau = -frameSeconds / ln h; so the share for a time constant is exp(-frameSeconds / tau). A time constant of 0 keeps
* nothing (0: take the new value).
*/
float hysteresisForTimeConstant(float frameSeconds, float timeConstant);

/*
* An irradiance texel's hysteresis this frame, from the frame's and the change between its old and new stored values
* (the largest change of any channel): lowered by the drop past the change threshold, 0 past the reset threshold.
*/
float irradianceTexelHysteresis(float hysteresis, const Vec3& oldEncoded, const Vec3& newEncoded);

// Keep hysteresis of the old value and take the rest from the new.
Vec3 blendProbeValue(const Vec3& oldValue, const Vec3& newValue, float hysteresis);
Vec2 blendProbeValue(const Vec2& oldValue, const Vec2& newValue, float hysteresis);

// Throws std::invalid_argument for a volume the renderer cannot hold: fewer than one probe along an axis, spacing not
// positive, more than maxProbesPerVolume probes, or atlases larger than maxProbeAtlasSide.
void validateProbeVolume(const ProbeVolume& volume);

int probeCount(const ProbeCounts& counts);

// Probes are numbered x first, then y, then z.
int probeIndex(const ProbeCounts& counts, int x, int y, int z);
Vec3 probePosition(const ProbeVolume& volume, int index);

/*
* Every probe's bordered square side by side in one image per kind (an atlas): probe i's square is column
* i mod (counts.x * counts.y), row i / (counts.x * counts.y), so each row of squares is one layer of the grid (one z).
*/
struct ProbeAtlasSize {
    int width = 0;
    int height = 0;
};

ProbeAtlasSize probeAtlasSize(const ProbeCounts& counts, int side);

// The top-left texel of probe index's bordered square (side + 2 across).
TexelCoord probeTileOrigin(const ProbeCounts& counts, int index, int side);

// Where to sample probe index's atlas for a direction: 0..1 across the whole atlas, inside the square's inner texels.
Vec2 probeAtlasUv(const ProbeCounts& counts, int index, const Vec3& direction, int side);

/*
* What every probe pass reads, as push constants (must match ProbeVolumeData in assets/shaders/ddgi_volume.glsl):
* exactly the 128 bytes every GPU allows.
*/
struct GpuProbeVolume {
    float origin[4];      // xyz the first probe
    float spacing[4];     // xyz the spacing between probes, w the longest distance recorded (probeMaxDistance)
    std::int32_t counts[4]; // xyz probes along each axis, w rays per probe this frame
    float rotation[3][4]; // this frame's ray rotation, its matrix's rows in xyz
    float sky[4];         // rgb the light rays that hit nothing bring back (the sky behind everything)
    float hysteresis[4];  // x the irradiance's, y the distances' share kept this frame, z the debug balls' radius
};

static_assert(sizeof(GpuProbeVolume) == 128, "GpuProbeVolume must match ddgi_volume.glsl and fit in 128 bytes of push constants");

// What changes per frame beside the volume itself.
struct ProbeFrame {
    int raysPerProbe = 64;
    Quat rotation;
    Vec3 sky{0.0f, 0.0f, 0.0f};
    float irradianceHysteresis = 0.0f;
    float distanceHysteresis = 0.0f;
    float ballRadius = 0.1f;
};

GpuProbeVolume packProbeVolume(const ProbeVolume& volume, const ProbeFrame& frame);
