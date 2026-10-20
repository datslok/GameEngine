#pragma once

#include "core/settings_visitor.h"
#include "math/quat.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/ddgi_shared.h"
#include "scene/probe_volume.h"

#include <array>
#include <cstdint>
#include <vector>

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
inline constexpr int probeRayCycle = DDGI_RAY_CYCLE;
inline constexpr float probeDistanceSharpness = static_cast<float>(DDGI_DISTANCE_SHARPNESS);
inline constexpr float backFaceDistanceShare = static_cast<float>(DDGI_BACK_FACE_DISTANCE_SHARE);
inline constexpr float minProbeTexelWeight = static_cast<float>(DDGI_MIN_TEXEL_WEIGHT);
inline constexpr float maxDistanceDiagonals = static_cast<float>(DDGI_MAX_DISTANCE_DIAGONALS);
inline constexpr int maxProbeAtlasSide = DDGI_MAX_ATLAS_SIDE;
inline constexpr float probeChangeFloorShare = static_cast<float>(DDGI_CHANGE_FLOOR_SHARE);
inline constexpr int maxProbeHistory = DDGI_MAX_HISTORY;

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

    /*
    * A ray budget: the still probes trace raysPerProbe rays this many times a second at most, so above that frame rate
    * each frame traces a share of them (raysPerProbe halved or quartered, on a ladder, probeRayDivisor), and the probe
    * work stops growing with the frame rate. Noise goes with rays a second, so 240 fps on a budget of 60 flickers like
    * 60 fps does without one (the dark room texel of ddgi_test: 0.95% against 0.96%, and 0.33% at 240 fps with all 64),
    * and the light settles as fast either way, since each frame still blends by its own time. Probes near fast lights
    * keep fastRaysPerProbe. 0 traces raysPerProbe every frame.
    */
    float probeUpdatesPerSecond = 0.0f;

    // How long the stored light takes to follow a change: after this many seconds, 1/e (37%) of a step change is left,
    // at any frame rate. Shorter reacts faster and flickers more: noise goes as 1/sqrt(rays x time constant). 2 s was
    // chosen by eye in the light lab's dark back room, where 0.5 s crawled visibly; the price is lag, a light switched
    // on or off bringing its bounce light in over a few seconds, and moving lights and objects trailing theirs (scene
    // events, planned, will speed probes up for such changes). Distances are kept steadier, since flickering visibility
    // is worse than slightly late visibility [2021].
    float irradianceTimeConstant = 2.0f;
    float distanceTimeConstant = 1.0f;

    /*
    * The adaptive memory (adaptiveProbeBlend): each irradiance texel keeps the long memory above while its light only
    * wobbles within its own measured noise, and restarts its average from the last changeWindowSeconds when the light
    * moves more than changeThreshold standard deviations of that window's mean, so a steady room can use a long memory
    * (calm) and a lamp switched on still comes in fast. In the dark room model (ddgi_test), with a 4 s long memory
    * against the fixed 2 s: a lamp switched on or off comes in within about 0.5-1.1 s instead of 4.6 and the light
    * crawls half as much (8 s: a quarter); but a change too faint to tell from the noise in a few frames (a dim patch's
    * light doubling, or a lamp switched off where the probe saw it with a fraction of a ray a frame, whose noise takes
    * the long memory to fade) is only followed by the long memory, slower than 2 s, and so is a slow drift. Fast
    * lights' atlas and the distances keep their fixed memories.
    */
    bool adaptiveMemory = false;
    float changeWindowSeconds = 0.2f;
    float changeThreshold = 5.0f;

    /*
    * Sharing between neighbouring probes: each probe's irradiance (still and fast) is read as its own light weighted 1
    * and each of its six face neighbours' that it can see weighted probeSharing (sharedProbeValue), so neighbours,
    * which see nearly the same light, pool their rays: like several times the rays, with no more tracing and no lag,
    * for bounce light a little blurrier (about two cells across instead of one). Each probe then also turns its rays
    * its own way (probeRayTurn), so neighbours' errors are independent; with one shared turn they would all err alike
    * and sharing would remove nothing. The probes keep accumulating their own unshared light; only what is read (the
    * main pass, the debug balls, the next frame's bounce) is shared. 0: off, everything exactly as before.
    */
    float probeSharing = 0.0f;

    /*
    * Fast lights (PointLight::fast, SpotLight::fast: flashlights, headlights) keep their light in a second irradiance
    * atlas with a much shorter memory, so their bounce follows them instead of trailing for seconds; light adds up, so
    * the two atlases are simply summed where they are read. 0.05 s kept up with the light lab's flashlight by eye, for
    * a little flicker (in the back room, 0.55% of change in 0.2 s at the 90th percentile at 240 frames a second, 1.6%
    * at 60, against 0.25% for the still light alone; 0.2 s would add none, for a longer trail). Probes within reach of a
    * fast light trace fastRaysPerProbe rays, since a short memory averages fewer frames (the rest still trace
    * raysPerProbe); 64 flickered over three times as much. In the dense probe test every probe is within the
    * flashlight's reach, and 256 rays cost about 2 ms more a frame than 64.
    */
    float fastIrradianceTimeConstant = 0.05f;
    int fastRaysPerProbe = 256;

    // Keep fast lights apart as above. Off, they are lights like any other: their bounce goes into the still atlas under
    // its memory (with the adaptive memory, restarting when it changes enough to tell from the noise), no probe traces
    // the fast count, and the fast atlas fades to nothing.
    bool fastLights = true;

    // The fast blend never takes a frame as longer than this, so one slow frame (a hitch) cannot replace most of its
    // history with one noisy estimate.
    float fastBlendMaxFrameSeconds = 1.0f / 30.0f;

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

// The inner texel a direction falls in (the inverse of probeTexelDirection), clamped inside the square.
TexelCoord probeDirectionTexel(const Vec3& unitDirection, int side);

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
* A rotation drawn uniformly from all rotations (Shoemake 1992), the same for the same number: every probe's rays are
* turned by it (one per ray cycle, probeRayFrame), so the spiral's gaps move and the samples add up to the whole sphere
* over time, with no direction favoured.
*/
Quat randomProbeRotation(std::uint64_t frameNumber);

// The rotation as the shaders take it: its matrix's rows.
struct ProbeRotationRows {
    Vec3 rows[3];
};

ProbeRotationRows probeRotationRows(const Quat& rotation);

/*
* Which rays a frame traces: frames take turns through one spiral of count x probeRayCycle points, frame number f taking
* phase f mod probeRayCycle, and a cycle's frames share one rotation (randomProbeRotation of the cycle's number), so
* together they cover the sphere evenly rather than as unevenly as independent random rotations would. In a room lit by
* a small bright patch, how many rays find it then barely varies from cycle to cycle, which is what flickered. Probes
* near fast lights trace more rays on the same cycle: fitting shorter cycles to their short memory (a power of two, the
* phase modulo it, under the same rotation) measured worse at 60, 120 and 240 frames a second, since a short cycle traces
* the same directions again and again until the rotation turns.
*/
struct ProbeRayFrame {
    Quat rotation;
    int phase = 0;
};

ProbeRayFrame probeRayFrame(std::uint64_t frameNumber);

// Ray ray of a probe's count in a frame of this phase: spiral point ray x probeRayCycle + phase, turned by the rotation.
Vec3 probeRayDirection(int ray, int count, int phase, const ProbeRotationRows& rotation);

// The same, turned by the probe's own turn first (probeRayTurn), then by the frame's rotation.
Vec3 probeRayDirection(int ray, int count, int phase, const ProbeRotationRows& rotation, const ProbeRotationRows& turn);

/*
* Probe number probe's own turn of its rays, the same every frame: a uniform rotation (randomProbeRotation of a number
* no ray cycle reaches), so neighbouring probes trace different directions and their errors are independent, which is
* what lets sharing between them average the errors away.
*/
ProbeRotationRows probeRayTurn(int probe);

// Each probe's turn as the shaders read it (std430: three vec4 rows, w unused).
struct GpuProbeTurn {
    float rows[3][4];
};

static_assert(sizeof(GpuProbeTurn) == 48, "GpuProbeTurn must match ProbeTurns in ddgi_volume.glsl");

// Every probe's turn: its own (probeRayTurn) when sharing, else identity rows, which leave every ray exactly as before.
std::vector<GpuProbeTurn> packProbeTurns(int probes, bool independent);

// Whether a probe sees its neighbour separation away, from its mean distance towards it (DDGI_SHARE_VISIBLE_SHARE).
bool probesSeeEachOther(float meanDistance, float separation);

/*
* A probe's light as sharing reads it: its own weighted 1, each visible neighbour's weighted weight, over the sum of the
* weights, so uniform light stays exactly itself.
*/
Vec3 sharedProbeValue(const Vec3& own, const std::array<Vec3, 6>& neighbours, const std::array<bool, 6>& visible, float weight);

/*
* What the sharing pass reads, as push constants (must match ddgi_share.comp): the grid and the weight.
*/
struct GpuProbeShare {
    std::int32_t counts[4]; // xyz probes along each axis
    float spacing[4];       // xyz the spacing between probes
    float weight[4];        // x the sharing weight
};

static_assert(sizeof(GpuProbeShare) == 48, "GpuProbeShare must match ddgi_share.comp");

GpuProbeShare packProbeShare(const ProbeVolume& volume, float weight);

/*
* Whether a probe counts as within reach of a light, for tracing more rays: the light's range, grown by reach (the
* longest distance the probes record), reaches the probe, as the hit lights are chosen (ddgi_sampling.h, probeHitLights).
*/
bool probeNearLight(const Vec3& probe, const Vec3& light, float range, float reach);

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

/*
* Keeping a share h of the old value each frame relaxes exponentially, like a capacitor charging, with time constant
* tau = -frameSeconds / ln h; so the share for a time constant is exp(-frameSeconds / tau). A time constant of 0 keeps
* nothing (0: take the new value).
*/
float hysteresisForTimeConstant(float frameSeconds, float timeConstant);

/*
* Keep hysteresis of the old value and take the rest from the new. Irradiance is blended as it is, linearly, not as
* 2021's fifth roots with per-texel speed-ups: with few rays reaching light (a dark room seeing a sunlit doorway), the
* mean of the roots of noisy estimates settled at half the true light, and the noise tripped the speed-ups, which
* passed it straight to the screen as flicker (ddgi_test, the sparsely lit texel).
*/
Vec3 blendProbeValue(const Vec3& oldValue, const Vec3& newValue, float hysteresis);
Vec2 blendProbeValue(const Vec2& oldValue, const Vec2& newValue, float hysteresis);

// How bright a probe's light looks: its luminance (Rec. 709 weights, as bloom's Karis average).
float probeLuminance(const Vec3& light);

/*
* An irradiance texel as the adaptive memory keeps it: the stored light and how many frames it averages since its last
* restart (in the irradiance atlas, alpha), the change window's mean light and the standard deviation of one frame's
* luminance about it (in the change atlas). history 0: never updated adaptively (fresh, or just switched on).
*/
struct AdaptiveProbeTexel {
    Vec3 stored{0.0f, 0.0f, 0.0f};
    float history = 0.0f;
    Vec3 recent{0.0f, 0.0f, 0.0f};
    float noise = 0.0f;
};

/*
* One frame of the adaptive memory, a running mean that restarts when the light really changes (like a denoiser's
* history length). The stored light blends the estimate in by the smaller of 1 / (history + 1) and the long memory's
* share (longKeep), so after a restart it is the plain mean of the frames since, until that would keep more than the
* long memory. The window's mean blends by windowKeep, and the noise (the variance of each frame's luminance about the
* window's mean) by longKeep. When the window's mean and the stored light differ by more than threshold times the
* window's own spread (an exponential mean keeping k has the variance sigma^2 (1 - k) / (1 + k)) plus
* probeChangeFloorShare of the light, the light has changed: the stored light becomes the window's mean, worth the
* k / (1 - k) frames whose running mean keeps the window's share. A texel without history starts from its stored light
* with the history whose running mean keeps longKeep, so it carries on smoothly (fresh atlases: shares 0, the first
* estimate taken outright).
*/
AdaptiveProbeTexel adaptiveProbeBlend(const AdaptiveProbeTexel& texel, const Vec3& estimate, float longKeep, float windowKeep, float threshold);

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
* exactly the 128 bytes every GPU allows, so the ray counts fill the rotation rows' spare w slots (whole numbers, exact
* as floats).
*/
struct GpuProbeVolume {
    float origin[4];      // xyz the first probe, w the fast irradiance's share kept this frame
    float spacing[4];     // xyz the spacing between probes, w the longest distance recorded (probeMaxDistance)
    std::int32_t counts[4]; // xyz probes along each axis, w the ray stride: room for the most rays any probe traces
    float rotation[3][4]; // this frame's ray rotation, its matrix's rows in xyz; w: still probes' rays, fast probes' rays, the change window's share kept
    float sky[4];         // rgb the light rays that hit nothing bring back (the sky behind everything), w the change threshold (0: fixed memory)
    float hysteresis[4];  // x the irradiance's, y the distances' share kept this frame, z the debug balls' radius, w the ray phase
};

static_assert(sizeof(GpuProbeVolume) == 128, "GpuProbeVolume must match ddgi_volume.glsl and fit in 128 bytes of push constants");

// What changes per frame beside the volume itself.
struct ProbeFrame {
    int raysPerProbe = 64;     // probes that no fast light reaches
    int fastRaysPerProbe = 64; // probes within reach of a fast light
    Quat rotation;
    int rayPhase = 0; // the frame's place in its ray cycle (ProbeRayFrame)
    Vec3 sky{0.0f, 0.0f, 0.0f};
    float irradianceHysteresis = 0.0f;
    float fastIrradianceHysteresis = 0.0f;
    float distanceHysteresis = 0.0f;
    float windowHysteresis = 0.0f; // the adaptive memory's change window's share kept
    float changeThreshold = 0.0f;  // 0: the fixed memory
    float ballRadius = 0.1f;
};

// Room for each probe's rays in the ray buffers: the larger count.
int probeRayStride(const ProbeFrame& frame);

GpuProbeVolume packProbeVolume(const ProbeVolume& volume, const ProbeFrame& frame);

// The ray budget's frame time is smoothed over this long, so one quick or slow frame does not move the ladder.
inline constexpr float rayBudgetSmoothingSeconds = 0.25f;

// A step up the ladder is taken this share short of it, so a frame rate jittering round a step (a 240 Hz cap) settles
// on it, at most this share under the budget's rays a second; a step down waits for the frame rate to fall the larger
// share below the step, so the count cannot flip every frame.
inline constexpr float rayBudgetStepUpShare = 0.05f;
inline constexpr float rayBudgetStepDownShare = 0.1f;

/*
* What the still probes' rays are divided by this frame, for a ray budget of updatesPerSecond (0: none, 1): about the
* largest power of two not above framesPerSecond / updatesPerSecond, so the rays a second stay at or near the budget's,
* and never so large that raysPerProbe / divisor falls below minRaysPerProbe. The steps up and down are apart
* (rayBudgetStepUpShare, rayBudgetStepDownShare).
*/
int probeRayDivisor(int currentDivisor, float framesPerSecond, float updatesPerSecond, int raysPerProbe);

/*
* What the probes carry from frame to frame beside their atlases: the number of the next frame, for its rays, and the
* ray budget's smoothed frame time (0 before the first frame) and divisor.
*/
struct ProbeSchedule {
    std::uint64_t frameNumber = 0;
    float smoothedFrameSeconds = 0.0f;
    int rayDivisor = 1;
};

/*
* This frame's rays and blending (all of ProbeFrame but the sky), moving the schedule on a frame: ray counts clamped to
* what a probe may trace, the still count divided for the ray budget, the frame's place in the ray cycle, and the shares kept from the time constants, the fast
* one's frame time capped. Without fast lights near the volume (fastLights false) every probe traces the still count.
* fresh: atlases just made, which keep nothing.
*/
ProbeFrame scheduleProbeFrame(ProbeSchedule& schedule, const GlobalIlluminationSettings& settings, float frameSeconds, bool fastLights, bool fresh);
