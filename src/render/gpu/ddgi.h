#pragma once

#include "core/settings_visitor.h"
#include "math/quat.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/ddgi_shared.h"
#include "scene/probe_volume.h"

#include <array>
#include <cstdint>
#include <span>
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
inline constexpr float probeSeedFrames = static_cast<float>(DDGI_SEED_FRAMES);
inline constexpr int maxProbeSeeds = DDGI_MAX_SEEDS;

// The most probes the atlases may hold, every cascade together: one workgroup per probe, and every GPU runs at least
// 65,535 groups in a row.
inline constexpr int maxProbesPerVolume = 65535;

/*
* Everything about global illumination that a game, the debug UI or a player may tune. Ray tracing only: without ray
* queries there are no probes yet (a software tracer comes later), and nothing here applies.
*/
struct GlobalIlluminationSettings {
    // Trace and update the probes round the frame's ProbeCascades.
    bool enabled = true;

    /*
    * Where the probes are (render/gpu/probe_cascades.h): probeCascades nested grids round the point the game's
    * ProbeCascades follows (the camera), the densest probeSpacing apart and each further one twice as far apart, so
    * twice as far across. The densest has densestProbesAcross probes along both level axes and densestProbesHigh up,
    * the coarser ones cascadeProbesAcross and cascadeProbesHigh: detail matters most near the camera, so the densest is
    * the one made larger. Each lights points at full weight out to a little over a third of its width from the camera
    * (probeCascadeReach), so the densest, 31 x 17 probes half a metre apart, covers 6.1 m round the camera at full
    * weight and 2.6 m up and down (a room round the camera, and its floor and ceiling from eye height; 21 x 13 covered
    * 3.6 m and 1.6 m), the next 7.3 m and 3.4 m, the fourth 29 m and 13 m, and the two far ones, 8 m and 16 m apart,
    * 58 m and 116 m (terrain-scale bounce and some sky blocking: valleys, forests, the shady side of big buildings;
    * beyond them only the flat sky ambient). Finer probes see light through doorways that coarser ones miss; each
    * cascade costs memory (about 9 KB a probe: 45,000 probes, about 400 MB) and tracing (only probes near surfaces
    * trace, so the far cascades' few near the ground cost little). Odd counts centre the camera best. Changing any of them rebuilds the probes (a hitch).
    */
    int probeCascades = 6;
    float probeSpacing = 0.5f;
    int densestProbesAcross = 31;
    int densestProbesHigh = 17;
    int cascadeProbesAcross = 21;
    int cascadeProbesHigh = 13;

    /*
    * A probe that starts (brought in by a cascade's move, woken, newly vigilant) begins from the coarser cascades' light
    * where it stands, as a few frames of its own (probeSeedFrames), instead of from its first frame's rays alone. In the
    * dark room model (probe_cascades_test, 128 rays at 60 fps) its light errs 9.6% over its first quarter second with a
    * seed 20% off, against 13.3% from its rays alone; but 24% with a seed twice or none of the light (a coarse probe that
    * sees through a doorway the fine one cannot), and from half a second on all are within half a percent of each other.
    * Off: every probe starts from its rays.
    */
    bool seedProbes = true;

    // Rays each probe traces per frame. More converge faster and flicker less; the time constants below average over
    // rays per second of history, so a slower GPU can trace fewer with longer time constants.
    int raysPerProbe = 128;

    /*
    * A ray budget: the still probes trace raysPerProbe rays this many times a second at most, so above that frame rate
    * each frame traces a share of them (raysPerProbe halved or quartered, on a ladder, probeRayDivisor), and the probe
    * work stops growing with the frame rate. Noise goes with rays a second, so 240 fps on a budget of 60 flickers like
    * 60 fps does without one (the dark room texel of ddgi_test: 0.95% against 0.96%, and 0.33% at 240 fps with all 64),
    * and the light settles as fast either way, since each frame still blends by its own time. Probes near fast lights
    * keep fastRaysPerProbe. 0 traces raysPerProbe every frame.
    */
    float probeUpdatesPerSecond = 60.0f;

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
    bool adaptiveMemory = true;
    float changeWindowSeconds = 0.2f;
    float changeThreshold = 8.0f;

    // Scene events (ProbeEvent, signalled by the game: a lamp switched, the clock jumped): the probes in an event's region
    // restart their average at once (advanceProbeRestarts). Off, events are ignored and only the memories above follow
    // a change.
    bool sceneEvents = true;

    // While an event's restart runs, the restarted probes share their still light with this weight at first, fading back
    // to probeSharing (the preset's) over the restart's period (eventProbeSharingWeight): averaging few frames, they are
    // noisiest just then. Never less than probeSharing. Needs sharing on (either weight above 0), as every preset has.
    float eventProbeSharing = 0.5f;

    /*
    * A soft restart, for restarts that take no event check (the fixed memory, with the adaptive memory off, and the
    * distances): through an event's restart, the restarted probes keep at least the share of their old light that a
    * memory this long keeps each frame (restartedProbeKeep), so the first frames after it are averages, not single
    * noisy estimates; 0 is a hard restart, the new light's first frame taken outright. With the adaptive memory, the
    * event check spares unchanged light instead, and restarted texels start from a mean of frames. The price: the old light fades
    * out over about this long plus a tail (a running mean keeps its first samples), so a switch takes longer to arrive.
    * In the dark room model at 30 fps (ddgi_test), a probe whose light did not change errs 22.5% over the first half
    * second restarted hard, 16.9% at 0.05 s, 13.4% at 0.1 s, 8.1% at 0.3 s (1.4% with no event at all), while a switch
    * takes 0.05, 0.14, 0.26 and 1.45 s to reach 90%. So keep regions to where the light changes: even softly, a
    * restarted probe whose light stays is far noisier for a moment than one left alone.
    */
    float eventMemorySeconds = 0.05f;

    // How fast a restart's memory grows back: its time constant is this share of the time since the event, until the
    // usual memory is the shorter (restartedProbeKeep). 1 is close to the plain mean since the event, which keeps its
    // first frames as long as any: the lamp light still in them (a soft restart's share, the bounce of last frame's
    // probes) faded only as 1 / time, so a lamp switched off took seconds to go quite dark. Smaller forgets the first
    // frames sooner, for more noise while the memory grows. In the dark room model at 30 fps (ddgi_test, the fixed
    // memory with the default soft restart), switched off, within 1% of darkness after 1.32 s at 1, 0.30 s at 0.5,
    // 0.20 s at 0.25 (the flashlight: 0.46 s); a restarted probe whose light stays errs 3.3%, 4.5% and 7.2% from half a
    // second to 4 s after (1.6% left alone). It also forgets the bounce of last frame's probes that the first frames
    // after a switch still hold, which the model leaves out.
    float eventMemoryGrowth = 0.5f;

    /*
    * The event check (adaptive memory only; adaptiveProbeUpdate): for eventCheckSeconds after an event each texel in its
    * region compares the mean of the frames since with its stored light, in units of its own noise shrinking as
    * 1 / sqrt(frames), and restarts only past eventChangeThreshold of them. A big change restarts at once, a smaller one
    * once enough frames tell it from the noise ((threshold x noise / change)^2 frames), and light that hardly changed
    * keeps its long memory, so dim areas the event barely touches do not flicker. 0 restarts every texel at once. In
    * the dark room model at 30 fps (ddgi_test): a texel the lamp makes 15% brighter, or leaves alone, flickers 0.74% over
    * the 2 s after an event, as with no events, against 13.9% and 14.9% restarted outright; the lamp itself 90% on in
    * 0.16 s and within 1% of darkness 0.28 s after switching off (no events: 5.2 and 18.3 s). A threshold of 3 gave
    * 0.09 and 0.13 s, 5 gave 1.3 and 4.9 s (too few frames within the check to tell a sparsely lit texel's change).
    */
    float eventChangeThreshold = 4.0f;
    float eventCheckSeconds = 0.5f;

    /*
    * Sharing between neighbouring probes: each probe's irradiance (still and fast) is read as its own light weighted 1
    * and each of its six face neighbours' that it can see weighted probeSharing (sharedProbeValue), so neighbours,
    * which see nearly the same light, pool their rays: like several times the rays, with no more tracing and no lag,
    * for bounce light a little blurrier (about two cells across instead of one). Each probe then also turns its rays
    * its own way (probeRayTurn), so neighbours' errors are independent; with one shared turn they would all err alike
    * and sharing would remove nothing. The probes keep accumulating their own unshared light; only what is read (the
    * main pass, the debug balls, the next frame's bounce) is shared. 0: off, everything exactly as before.
    */
    float probeSharing = 0.1f;

    // The same for fast lights' light (the fast atlas) on its own weight: their short memory averages few frames, so
    // their bounce flickers most at low frame rates (a flashlight at 30 fps), and sharing makes up for it. The quality
    // presets leave it alone. Either weight above 0 turns every probe's rays its own way.
    float fastProbeSharing = 0.5f;

    /*
    * Fast lights (PointLight::fast, SpotLight::fast: flashlights, headlights) keep their light in a second irradiance
    * atlas with a much shorter memory, so their bounce follows them instead of trailing for seconds; light adds up, so
    * the two atlases are simply summed where they are read. 0.05 s kept up with the light lab's flashlight by eye, for
    * a little flicker (in the back room, 0.55% of change in 0.2 s at the 90th percentile at 240 frames a second, 1.6%
    * at 60, against 0.25% for the still light alone; 0.2 s would add none, for a longer trail). Probes within reach of a
    * fast light trace fastRaysPerProbe rays, since a short memory averages fewer frames (the rest still trace
    * raysPerProbe); 64 flickered over three times as much. In the dense probe test every probe is within the
    * flashlight's reach, and 256 rays cost about 2 ms more a frame than 64. 128 by default (the High quality preset),
    * 256 on Ultra.
    */
    float fastIrradianceTimeConstant = 0.1f;
    int fastRaysPerProbe = 128;

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

    /*
    * Relocation (render/gpu/probe_relocation.h): probes move off the static level, out of walls and away from surfaces
    * they nearly touch, within their own cells, once at start-up and after a geometry change. Off, every probe stays
    * where the grid puts it.
    */
    bool probeRelocation = true;

    /*
    * Probe states: only probes that some surface reads are traced (vigilant: a static surface within their cells; awake:
    * something that moves within wakeCells more cells), and probes buried in the level are never read. Off, every probe
    * is traced and read, as before.
    */
    bool probeStates = true;
    float wakeCells = 1.0f;

    // A debug view: each probe of one cascade (0 the densest) drawn as a small ball showing the light it stores, in the
    // direction of the ball's surface, as much larger than the densest's balls as its spacing is. One at a time: the
    // cascades nest, so a coarser probe sits where finer ones do.
    bool showProbes = false;
    int shownProbeCascade = 0;
    float probeBallRadius = 0.1f;
};

void visit(GlobalIlluminationSettings& settings, SettingsVisitor& visitor);

// A probe's place in a grid, counted from the first probe along each axis.
struct ProbeGridCoord {
    int x = 0;
    int y = 0;
    int z = 0;
};

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
* What the sharing pass reads, as push constants (must match ddgi_share.comp): the atlases' grid, how many probes and
* cascades there are (where each cascade is, its size and how it is scrolled, in the cascades' buffer), and the weights.
*/
struct GpuProbeShare {
    std::int32_t counts[4]; // xy the atlases' columns and rows, z every cascade's probes together, w how many cascades
    float weight[4];        // x the still light's sharing weight, y the fast light's, z a restarted probe's at first, w the long memory's share kept
};

static_assert(sizeof(GpuProbeShare) == 32, "GpuProbeShare must match ddgi_share.comp");

GpuProbeShare packProbeShare(int probes, int cascades, float weight, float fastWeight, float eventWeight, float longKeep);

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

/*
* A texel the event check found changed: its light and the change window's mean become the mean of the frames since the
* event, its history -1 (marked negative while the restart runs, counting its frames since), its noise measured afresh.
*/
AdaptiveProbeTexel restartProbeTexel(const Vec3& eventMean);

/*
* A frame of a texel an event restarted, while its frames since (its history, positive here) are fewer than the change
* window holds: the stored light blends by keep (the restart's growing memory), the window's mean is the stored light,
* and the noise the mean square of each frame's luminance about the mean before it, measured afresh, since the old
* light's noise is no guide to the new one's. No change is looked for: the event check found it. Looking, with the old
* noise (a dark room's, tiny) or the first frames' few samples, the lit room's own noise passed for change after change,
* each restarting the average from a window that still held the darkness: in the dark room model a lamp came in in
* 0.27 s instead of 0.04.
*/
AdaptiveProbeTexel restartedProbeBlend(const AdaptiveProbeTexel& texel, const Vec3& estimate, float keep);

/*
* A frame of the event check, framesSinceEvent (0 or more) frames after an event, for a texel it has not restarted: the
* change window's mean becomes the mean of the frames since the event, the stored light and the noise blend as the
* adaptive memory's (the long memory), and when the mean since the event and the stored light differ by more than
* eventThreshold times the noise measured before (shrinking as 1 / sqrt(frames since + 1)) plus probeChangeFloorShare of
* the light, the texel restarts from that mean (restartProbeTexel).
*/
AdaptiveProbeTexel eventCheckBlend(const AdaptiveProbeTexel& texel, const Vec3& estimate, float longKeep, int framesSinceEvent, float eventThreshold);

/*
* The event check lasts at least this many frames, however short eventCheckSeconds is at the frame rate: a sparsely lit
* texel whose light goes out needs about (threshold x noise / light)^2 = (4 x 0.9)^2, 13, frames to tell it from its
* noise, so at 15 fps a 0.5 s check (7 frames) never decided, and the light took the long memory, 18 s in the dark room
* model, to go dark.
*/
inline constexpr float minEventCheckFrames = 16.0f;

// The event check's length in frames for its length in seconds at a frame time: at least minEventCheckFrames.
float eventCheckFrames(float checkSeconds, float frameSeconds);

// What the event check and a restart need, per probe (GpuProbeRestart::check, the shader's vec4).
struct ProbeEventCheck {
    float threshold = 0.0f; // eventChangeThreshold: noise units a change must pass
    float growth = 1.0f;    // eventMemoryGrowth
    float frames = 0.0f;    // the check's length in frames (eventCheckSeconds)
};

/*
* One frame of an irradiance texel with the adaptive memory, given the frames since a scene event reached its probe
* (negative: none running). A texel this event restarted (history negative) blends with the restart's growing memory
* (restartedProbeKeep of its own frames since, with no soft restart: it restarted from a mean of frames, and keeping its
* first frame longer only kept that frame's noise longer, 24.2% against 22.8% in the dark room model): restartedProbeBlend while its frames since are fewer than the change
* window holds, then adaptiveProbeBlend with that memory. A new event restarts such a texel at once, unchecked: the light
* an event changed is most likely switched back by the next (a lamp switched on and off again), and its noise, measured
* over the few frames since its restart, is no basis for a check; checked, a sparsely lit texel toggled faster than its
* check could decide never restarted, and the lamp took the long memory to go dark (13-16 s in the dark room model). Any
* other texel runs the event check (eventCheckBlend) for check.frames frames after the event, then adaptiveProbeBlend.
* With no event running, adaptiveProbeBlend, a restarted texel's marker dropped.
*/
AdaptiveProbeTexel adaptiveProbeUpdate(const AdaptiveProbeTexel& texel, const Vec3& estimate, float longKeep, float windowKeep, float threshold,
                                       int framesSinceEvent, const ProbeEventCheck& check);

/*
* The share a probe keeps frames (n) after a scene event restarted it: none on the event's frame, then a memory whose
* time constant is growth times the time since, exp(-1 / (growth n)) (growth 1 is close to a running mean's
* n / (n + 1)), never less than the soft restart's shortKeep (eventMemorySeconds), until the usual share longKeep is the
* smaller. Each earlier frame's weight then falls as (its time / the time now)^(1 / growth), not the running mean's first
* power. Negative: no restart, longKeep.
*/
float restartedProbeKeep(float longKeep, float shortKeep, float growth, float frames);

/*
* A probe's still sharing weight framesSinceRestart frames after a scene event restarted it: eventWeight on the event's
* frame, fading linearly back to weight over the restart's period (historyForShare(longKeep) frames, as long as its
* ramp lasts), never below weight. Negative (no restart), or no period (longKeep 0): weight.
*/
float eventProbeSharingWeight(float weight, float eventWeight, int framesSinceRestart, float longKeep);

// Throws std::invalid_argument for a volume the renderer cannot hold: fewer than one probe along an axis, spacing not
// positive, or more than maxProbesPerVolume probes (any that fit, the atlases hold: probeAtlasGrid).
void validateProbeVolume(const ProbeVolume& volume);

int probeCount(const ProbeCounts& counts);

// Slots are numbered x first, then y, then z.
int probeIndex(const ProbeCounts& counts, int x, int y, int z);

/*
* The slot a volume keeps the probe at grid place grid in: (grid + scroll) mod counts along each axis (ProbeScroll), so a
* volume that never scrolls keeps probe (x, y, z) in slot probeIndex(x, y, z). Grid places outside the volume wrap round.
*/
int probeSlot(const ProbeVolume& volume, const ProbeGridCoord& grid);

// The grid place of the probe kept in slot slot (the inverse of probeSlot).
ProbeGridCoord probeGridCoord(const ProbeVolume& volume, int slot);

// Where the grid puts the probe kept in slot slot.
Vec3 probePosition(const ProbeVolume& volume, int slot);

/*
* Every probe's bordered square side by side in one image per kind (an atlas), every cascade's probes one after another
* whatever their sizes: probe i's square is column i mod columns, row i / columns, as text runs along lines. The rows
* used to be the grid's layers, which needs every cascade the same width and makes a wide one's rows too long for an
* image (31 x 31 distance squares are 17,298 texels).
*/
struct ProbeAtlasGrid {
    int columns = 1;
    int rows = 1;
};

ProbeAtlasGrid probeAtlasGrid(int probes);

struct ProbeAtlasSize {
    int width = 0;
    int height = 0;
};

ProbeAtlasSize probeAtlasSize(const ProbeAtlasGrid& grid, int side);

// The top-left texel of probe index's bordered square (side + 2 across).
TexelCoord probeTileOrigin(const ProbeAtlasGrid& grid, int index, int side);

// Where to sample probe index's atlas for a direction: 0..1 across the whole atlas, inside the square's inner texels.
Vec2 probeAtlasUv(const ProbeAtlasGrid& grid, int index, const Vec3& direction, int side);

/*
* What every probe pass reads, as push constants (must match ProbeVolumeData in assets/shaders/ddgi_volume.glsl):
* exactly the 128 bytes every GPU allows, so the ray counts fill the rotation rows' spare w slots (whole numbers, exact
* as floats). Where each cascade's probes are, and how many, is in the cascades' buffer (render/gpu/probe_cascades.h,
* GpuProbeCascade): probe number p belongs to the last cascade whose first probe is p or less (probeCascadeHolding),
* and the atlases hold every probe in that numbering (probeAtlasGrid).
*/
struct GpuProbeVolume {
    std::int32_t counts[4];   // xy the atlases' columns and rows, z unused, w the ray stride: room for the most rays any probe traces
    std::int32_t cascades[4]; // x how many cascades, y every cascade's probes together, zw unused
    float rotation[3][4]; // this frame's ray rotation, its matrix's rows in xyz; w: still probes' rays, fast probes' rays, the change window's share kept
    float sky[4];         // rgb the light rays that hit nothing bring back (the sky behind everything), w the change threshold (0: fixed memory)
    float hysteresis[4];  // x the irradiance's, y the distances' share kept this frame, z the debug balls' radius, w the ray phase
    float keeps[4];       // x the fast irradiance's share kept this frame, yzw unused
};

static_assert(sizeof(GpuProbeVolume) == 128, "GpuProbeVolume must match ddgi_volume.glsl and fit in 128 bytes of push constants");

// What changes per frame beside the volume itself.
struct ProbeFrame {
    int raysPerProbe = 128;     // probes that no fast light reaches
    int fastRaysPerProbe = 64; // probes within reach of a fast light
    Quat rotation;
    int rayPhase = 0; // the frame's place in its ray cycle (ProbeRayFrame)
    Vec3 sky{0.0f, 0.0f, 0.0f};
    float irradianceHysteresis = 0.0f;
    float fastIrradianceHysteresis = 0.0f;
    float distanceHysteresis = 0.0f;
    float windowHysteresis = 0.0f; // the adaptive memory's change window's share kept
    float restartHysteresis = 0.0f; // the soft restart's share kept at least (eventMemorySeconds)
    float restartGrowth = 1.0f;     // how fast a restart's memory grows back (eventMemoryGrowth)
    float restartThreshold = 0.0f;  // the event check's threshold (eventChangeThreshold)
    float restartCheckFrames = 0.0f; // the event check's length in frames (eventCheckSeconds)
    float changeThreshold = 0.0f;  // 0: the fixed memory
    float ballRadius = 0.1f;
};

// Room for each probe's rays in the ray buffers: the larger count.
int probeRayStride(const ProbeFrame& frame);

// probes: every cascade's together, which the atlases hold (probeAtlasGrid).
GpuProbeVolume packProbeVolume(int probes, int cascades, const ProbeFrame& frame);

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

/*
* Each probe's scene event this frame, as the update reads it (ProbeRestart in ddgi.glsl): the frames since an event
* restarted its irradiance and its distances, 0 on the event's frame, negative when none is running, the share of
* each it keeps this frame (restartedProbeKeep), and of the fast atlas, which restarts with the still irradiance but
* outright (no noise is measured there to check against, and its memory is short anyway; no soft restart, which would
* keep a switched-off light's bounce), and the event check's settings, worked out here since the update's push constants
* are full. A seeded probe (one that starts, given the coarser cascades' light where it stands: seed its slot in the
* frame's seeds) starts its stored light from the seed instead of its first frame's rays.
*/
struct GpuProbeRestart {
    std::int32_t irradianceFrames = -1;
    std::int32_t distanceFrames = -1;
    float irradianceKeep = 0.0f;
    float distanceKeep = 0.0f;
    ProbeEventCheck check;
    float fastKeep = 0.0f; // the fast atlas's share kept: it restarts with the still one, outright (the shader's check.w)
    std::int32_t seed = -1; // this frame's seed slot of a probe that starts from a seed, else -1
    std::int32_t unused[3] = {0, 0, 0};
};

static_assert(sizeof(GpuProbeRestart) == 48, "GpuProbeRestart must match ProbeRestart in ddgi.glsl");

// A probe that starts from a seed, and its seed's slot in the frame's seeds (the seed pass writes them, the update reads them).
struct ProbeSeed {
    int probe = 0;
    int slot = 0;
};

/*
* Move every probe's restart on a frame, then start this frame's events: the probes in an event's region (all of them
* when everywhere) restart their irradiance, and a geometry change their distances too; each then gets its share kept
* for the frame. A restart ends once its ramp (restartedProbeKeep, with the frame's restartHysteresis), counted from the
* end of the event check (texels may restart that late), has reached the frame's share kept, after which it would change nothing. A frame keeping none of an
* atlas (fresh atlases) starts no restart of it: there is nothing to forget. Sized to the volume: a new size starts with
* none running. The restarted probes (probe_relocation.h, restartedProbes: woken, newly vigilant or moved) restart
* their irradiance and distances outright: nothing kept on this frame, every texel restarted unchecked. Of those, the
* seeded ones (ProbeSeed) start their irradiance from the seed as if it were probeSeedFrames frames of their own: their
* restart counts from there, so the seed's share fades as their own rays come in (restartedProbeKeep). Distances are
* never seeded: a coarser probe's distances say nothing about what this one sees. restarts holds every cascade's probes
* (each cascade's, of its own size, one after another), each cascade's events tested against its own volume.
*/
void advanceProbeRestarts(std::vector<GpuProbeRestart>& restarts, std::span<const ProbeVolume> cascades, std::span<const ProbeEvent> events,
                          const ProbeFrame& frame, std::span<const int> restarted = {}, std::span<const ProbeSeed> seeded = {});

// One volume's probes alone, as the cascades' first.
void advanceProbeRestarts(std::vector<GpuProbeRestart>& restarts, const ProbeVolume& volume, std::span<const ProbeEvent> events, const ProbeFrame& frame,
                          std::span<const int> restarted = {}, std::span<const ProbeSeed> seeded = {});

/*
* A texel as the seed starts it: the seed as its stored light and its change window's mean, marked restarted
* (negative history) with probeSeedFrames frames behind it, its noise to be measured afresh.
*/
AdaptiveProbeTexel seededProbeTexel(const Vec3& seed);

// One restart's frames since its event, a frame on (keep: the share its atlas keeps this frame), or -1 once it has
// ended (advanceProbeRestarts' rule); negative stays negative.
std::int32_t advanceRestartFrames(std::int32_t frames, float keep, const ProbeFrame& frame);
