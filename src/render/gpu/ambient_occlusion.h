#pragma once

#include "core/settings_visitor.h"
#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "render/gpu/ambient_occlusion_shared.h"
#include "render/gpu/upscaling.h"

#include <cstdint>
#include <functional>
#include <optional>

/*
* Ambient occlusion: how much of the light from all around reaches a point past the small things next to it (a crease,
* the corner where a wall meets the floor, the gap under a block). The light from all around is E = integral of
* L(w) V(w) cos(theta) dw over the hemisphere: the light probes give L, and their own visibility catches walls and
* rooms, but they sit half a metre apart at best. Ambient occlusion is the cosine-weighted share of V within a small
* radius, 0 buried to 1 open, which the main pass multiplies the light from all around by (not the direct light, which
* has its shadows).
*
* Found by ground-truth ambient occlusion (GTAO, Jimenez, Wu, Pesce and Jarabo 2016), from the depth image alone:
* - a slice is the plane through the pixel containing the way to the camera, at an angle round it; on screen, a line
*   through the pixel. Walking along it both ways through the depth image, each side keeps its horizon: the highest
*   any surface rises (as the cosine of its angle from the way to the camera), each sample counting less towards the
*   radius's edge (the falloff), so the reach has no hard border;
* - between the two horizons the slice is open, and the cosine-weighted open arc has a closed form (sliceVisibility),
*   with the surface's normal projected into the slice;
* - averaged over slices turned round the way to the camera (each pixel's own, turned again every frame, which temporal
*   anti-aliasing averages; a small blur stands in without it).
* Light bouncing between a crease's walls fills it back in, more for bright ones (multiBounceOcclusion), and the
* reflection's stand-in is dimmed by Lagarde's fit from the occlusion and the roughness (specularOcclusion).
*
* The horizons are kept as a visibility bitmask (Therrien, Levesque and Gilet 2023): a horizon says everything below it
* is solid, so a thin pole blocks all the light behind it. Instead each slice's half circle is cut into AO_SECTORS
* sectors, a bit each, and a sample marks only the arc from its front to its back, a fixed thickness behind it along the
* view; a surface running on from the last sample fills the gap between them, so a wall is not left full of holes. Unlike
* the paper's even angles, the sectors here are equal shares of the slice's cosine-weighted light (sectorShare, from
* Jimenez's own closed form), so every bit weighs the same and no solid horizon loses the exact answer's weight.
*
* That also gives bounce light (screen-space global illumination). A marked sector does not see the light from all
* around: it sees the sample that marked it, and last frame's finished picture says how bright that is. So the slice
* gathers each sample's picture times the share of the light its newly marked sectors hold, only those a nearer sample
* has not taken. Open sectors get the light from all around, as before, and a white furnace adds up to exactly 1. The
* main pass adds the gathered light to the diffuse light from all around, and the surface's albedo scales both. As the
* picture holds last frame's bounce, each frame adds a bounce, a geometric series in the albedo, which converges.
* Only what is on screen can bounce, and a picture at the first frame or without anti-aliasing is not there to read.
*
* The pass runs between the depth prepass and the main pass (ambient_occlusion.comp). This is the API-free part,
* mirrored by assets/shaders/ambient_occlusion.glsl and checked against it by the GPU tests.
*/

struct AmbientOcclusionSettings {
    // On every GPU, with the depth prepass; off on the Low preset.
    bool enabled = true;

    // How far round a point its occluders count (metres): about the densest light probes' spacing, which handle what
    // is bigger.
    float radius = 0.5f;

    // The outer share of the radius over which an occluder's weight fades to nothing (above 0, at most 1).
    float falloff = 0.6f;

    // Slices a pixel takes each frame, and steps along each side of each: more is smoother and costs more.
    int slices = 3;
    int steps = 6;

    // How deep each sample is taken to be along the view (metres): light passes behind it. Too thin and walls turn
    // leaky; too thick and thin things cast halos.
    float thickness = 0.25f;

    // Bounce light gathered from last frame's picture (screen-space global illumination), needing anti-aliasing and
    // standing aside for the light probes, which already hold it.
    bool indirect = true;

    // How far round a point its samples reach while gathering (metres): bounce light carries further than creases.
    float indirectRadius = 2.0f;

    // The brightest a gathered sample counts for, in the picture's linear light: the sun's glint off a metal ball is
    // thousands of times brighter than what it stands for, and one such pixel would speckle whatever it lights.
    float indirectBrightest = 8.0f;

    // A crease's walls bouncing light back in, more for bright ones (Jimenez 2016's fit), while nothing is gathered.
    bool multipleBounces = true;

    // The reflection's stand-in dimmed in creases too (Lagarde 2014's fit).
    bool specularOcclusion = true;

    // Each pixel's result averaged over the frames before it, reprojected, before the blur and the main pass read it, so
    // the upscalers get steady light rather than noise to turn into grain and swirls. Needs anti-aliasing (whose frames
    // turn the noise).
    bool temporal = true;

    // How many frames the average holds at most: the noise falls as one over the square root of about twice this, and
    // a change in the light takes about this many frames to come through.
    int historyFrames = 16;

    // How far last frame's view depth at the reprojected texel may stray from the point's, as a share of it, before the
    // texel is taken for something else (a surface the point was hidden behind) and left out.
    float historyDepthShare = 0.05f;

    // The denoiser while gathering or without anti-aliasing: passes of the edge-aware a-trous filter (each twice as wide,
    // 2 reaching about 6 pixels each way, 4 about 30; 0 off), and how far a tap may lie off the pixel's surface plane, as
    // a share of its view depth, before it counts for little (a tap off by this much weighs 1/e). 2 by default: the user
    // saw no difference from 4 after the history's 16 frames, for about half the denoiser's cost.
    int denoisePasses = 2;
    float denoisePlaneShare = 0.02f;

    // Anti-lag: a pixel's history restarts when its fast average (3 frames) strays from its slow one by more than this
    // many of its own noise's deviations (a real change in the light, not noise), so a switched light's bounce follows
    // in a few frames instead of the history's.
    bool antiLag = true;
    float antiLagDeviations = 3.0f;

    // The history clip (variance clipping, Salvi 2016): before the new frame is blended in, the history is held within
    // the new frame's 3 x 3 neighbourhood mean plus or minus this many deviations of its spread, each channel on its own.
    // A history the neighbourhood no longer supports (a switched-off flashlight's bounce) is gone in a frame; steady
    // noise, whose box is as wide as the noise, is left alone. 0 turns it off.
    float historyClip = 2.0f;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(AmbientOcclusionSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a radius, indirect radius, thickness, brightest sample or history depth share not
// denoise plane share or anti-lag deviations not above 0, a history clip below 0, a falloff outside (0, 1], slices, steps or history frames outside 1 up to
// AO_MAX_SLICES, AO_MAX_STEPS and AO_MAX_HISTORY_FRAMES, or denoise passes outside 0 up to AO_DENOISE_MAX_PASSES.
void validateAmbientOcclusion(const AmbientOcclusionSettings& settings);

// An occluder's weight at this distance from the point: 1 up to the falloff's start, fading linearly to 0 at the radius.
float occluderWeight(float distance, float radius, float falloff);

/*
* A slice through the point: the axis it turns round (at right angles to the slice), the normal's angle from the way to
* the camera within it (positive towards the slice's side 0), and the length of the normal's projection into it.
*/
struct AoSlice {
    Vec3 axis;
    float normalAngle = 0.0f;
    float projectedLength = 0.0f;
};

// The slice containing the unit way to the camera and this direction (not along it), for a unit normal.
AoSlice aoSlice(const Vec3& normal, const Vec3& toCamera, const Vec3& direction);

// The horizon's cosine when nothing rises above the surface: side 0 (along the direction) or 1, the normal's angle plus
// or minus a right angle.
float lowHorizonCosine(const AoSlice& slice, int side);

// The cosine of the angle between the way to the camera and the way to a sample (its offset from the point).
float horizonCosine(const Vec3& offset, const Vec3& toCamera);

// The cosine-weighted open arc between the horizons (Jimenez 2016's closed form, clamped to the normal's hemisphere),
// times the projected normal's length: its average over slices round the way to the camera is the visibility.
float sliceVisibility(const AoSlice& slice, float horizonCosine0, float horizonCosine1);

// A gathered sample's light, brought down to the brightest it may be in its largest channel, keeping its colour.
Vec3 gatheredSampleLight(const Vec3& light, float brightest);

// Where a direction lies in the slice as a share of its cosine-weighted light, 0 at side 1's horizon to 1 at side 0's:
// the direction's horizon cosine, on this side of the way to the camera. Directions below the surface count as the horizon.
float sectorShare(const AoSlice& slice, float directionCosine, int side);

/*
* The bits of the sectors an arc between two shares covers, the shares either way round: each end rounded to a sector's
* edge at offset (0..1) past it, floor(share x AO_SECTORS + offset). At 0.5 a sector counts when at least half covered;
* with the offset random each frame, a sector counts as often as the share of it the arc covers, so the bits average to
* the arc exactly. Rounding at 0.5 alone made a sliver of a bright surface just above the horizon (a sunlit floor in the
* plane of the one gathering) mark a whole sector near the horizon, which is wide there, at some slice angles and not at
* others: rays of light no averaging removed.
*/
std::uint32_t sectorMask(float share0, float share1, float offset = 0.5f);

/*
* The step along a surface one pixel each way across the screen, from the pixel's point and its two neighbours on each
* side (before, after and the next ones beyond): of the two steps, the one whose next neighbour carries on in line with
* it, since a straight line of pixels across a plane is a straight line in space. So a pixel at an edge takes its own
* surface's slope, not the step to the other surface, which can be the shorter one (a wall's foot seen across a
* foreshortened floor). A step of no length (the neighbour off screen, clamped onto the pixel) gives way to the other.
*/
Vec3 surfaceStep(const Vec3& centre, const Vec3& before, const Vec3& beforeNext, const Vec3& after, const Vec3& afterNext);

// The surface's unit normal from its steps across and down the screen, facing the camera at the origin (the way to the
// camera where the steps do not span a plane).
Vec3 depthNormal(const Vec3& centre, const Vec3& across, const Vec3& downwards);

// How many pixels the radius spans at this view distance, for a pixel footprint per metre (screen_reflections.h,
// pixelFootprint), at most AO_MAX_SCREEN_RADIUS.
float aoScreenRadius(float radius, float footprint, float viewDistance);

/*
* The pass's three noises from the blue noise tile's texel at the pixel (tileNoise, its three channels) and the frame
* (0..1 each): x turns the slices, y staggers the steps, z offsets the sectors' rounding (sectorMask). Each channel moves
* on by its own irrational step a frame. They must be independent over the frames: a pixel sees a small bright thing only
* when its slice points at it, and a stagger tied to the angle lands its samples at the same distances from it every
* frame, drawing rings round it that no averaging removes; and blue across the screen, so what averaging leaves is fine
* grain rather than shapes (interleaved gradient noise, used first, left rings and rays that moved).
*/
Vec3 aoNoise(const Vec3& tileNoise, int frame);

// Step index of count's distance along a slice (pixels): spread quadratically (dense near the point, where creases are),
// staggered by noise (0..1), never under AO_MIN_SAMPLE_PIXELS.
float aoSampleDistance(int step, int count, float noise, float screenRadius);

/*
* The level of last frame's picture's pyramid (each level half the size, render size at 0) a gathered sample reads: its
* footprint is the spacing of the steps round it, the distance's slope 2 (step + noise) radius / count^2 pixels, and the
* level whose texels are that wide, log2 of it (0 below a pixel). A far sample then reads the mean of the stretch it
* stands for instead of one texel: a thin bright face between two steps no longer comes back whole one frame in twenty
* and not at all the rest (aliasing, sampling a picture finer than the steps), but a little every frame. The texture
* read clamps it to the pyramid's top.
*/
float aoSampleLevel(int step, int count, float noise, float screenRadius);

// The visibility seen by a matte surface of this albedo, light bouncing between the crease's walls (never darker).
Vec3 multiBounceOcclusion(float visibility, const Vec3& albedo);

// How much of the reflection's stand-in a crease lets through, from the view's cosine, the visibility and the roughness.
float specularOcclusion(float nDotV, float visibility, float roughness);

// The visibility the main pass uses: the screen's (1 without it) or the material's baked one (glTF's
// 1 + strength (texel - 1)), whichever is more closed. Both estimate the same open share, so taking their product would
// darken a crease both see twice.
float surfaceVisibility(float screenVisibility, float occlusionTexel, float occlusionStrength);

/*
* The denoiser: an edge-aware a-trous filter (Dammertz et al. 2010, as in SVGF). Each pass is the same 5 x 5 kernel with
* its taps stride = 2^pass pixels apart, so four passes reach about 30 pixels each way for 100 reads a pixel. Each tap's
* kernel weight is multiplied by two geometric checks against the centre: how far its point lies off the centre's
* surface plane, against the centre's view depth (aoPlaneWeight, so a sloping floor counts whole and a pillar in front
* or a wall behind does not), and how alike their normals are (aoNormalWeight, so nothing turns a corner). The light
* gathered is filtered by every pass; the visibility by the first AO_DENOISE_VISIBILITY_PASSES only.
*/

// The B3-spline kernel's weight at a tap dx, dy (each -2..2): (1, 4, 6, 4, 1) / 16 along each axis.
float atrousKernelWeight(int dx, int dy);

// exp(-|n . (tap - centre)| / (share x the centre's view depth)): 1 on the centre's plane, 1/e that share of the depth
// off it. Points are measured from the camera.
float aoPlaneWeight(const Vec3& centreNormal, const Vec3& centrePoint, const Vec3& tapPoint, float centreDepth, float share);

// max(0, a . b) to the power AO_DENOISE_NORMAL_POWER, for unit normals.
float aoNormalWeight(const Vec3& centreNormal, const Vec3& tapNormal);

// The point at view depth 1 along a pixel's ray (measured from the camera): the point at any view depth is this times it.
Vec3 aoDepthRay(const Mat4& inverseViewProjection, Vec2 pixel, Vec2 size);

// What the occlusion pass leaves for the denoiser at a pixel: its unit normal (from the depths) and its view depth, 0 for
// the sky and for points left open (radius under a pixel), which the denoiser leaves as they are and no tap reads.
struct AoGeometry {
    Vec3 normal;
    float viewDepth = 0.0f;
};

using AoGeometryImage = std::function<AoGeometry(int x, int y)>;
using AoLightImage = std::function<Vec4(int x, int y)>;


// The depth image as the pass reads it (reversed-Z), at a texel inside it.
using AoDepthImage = std::function<float(int x, int y)>;

// Last frame's picture at the point a texel inside the render shows, read at a level of its pyramid (aoSampleLevel);
// empty when there is nothing to gather.
using AoPicture = std::function<Vec3(int x, int y, float level)>;

// What the pass finds at a pixel: the open share, and the light gathered from what fills the rest (nothing without a
// picture).
struct AoLight {
    Vec3 gathered;
    float visibility = 1.0f;
};

// What the pass knows of the frame: the camera-relative view-projection's inverse, the render size, and a pixel's
// footprint per metre of view distance.
struct AoFrame {
    Mat4 inverseViewProjection;
    RenderSize size;
    float footprint = 0.0f;
};

// One pass of the denoiser at texel (x, y): rgb the light gathered, a the visibility, from the last pass's image (light).
Vec4 denoiseAoAt(int x, int y, int pass, const AoFrame& frame, const AmbientOcclusionSettings& settings, const AoGeometryImage& geometry,
                 const AoLightImage& light);

/*
* The pass's loop, as ambient_occlusion.comp runs it, at texel (x, y): open with nothing gathered for the sky and for
* points whose radius is under AO_MIN_SAMPLE_PIXELS on screen. sliceNoise turns the slices, stepNoise staggers the steps,
* sectorNoise offsets the sectors' rounding (0..1). Samples off screen or on the sky count as open. The reach is settings.radius; the pass record swaps in
* settings.indirectRadius while gathering, so give the mirror that radius to compare them.
*/
AoLight ambientOcclusionAt(int x, int y, const AoFrame& frame, const AmbientOcclusionSettings& settings, float sliceNoise, float stepNoise,
                           const AoDepthImage& depthAt, const AoPicture& picture = {}, float sectorNoise = 0.5f);

/*
* Temporal accumulation (ambient_occlusion_temporal.comp): each pixel's result is blended into a running average of the
* frames before it, found where the point was on last frame's screen. Each pixel keeps its average (the gathered light
* and the visibility), its view depth (clip w: the distance along the view, not to the eye) and its age, how many frames
* the average holds. A new frame weighs 1 / age, so while the age grows the average is the exact mean of every frame so
* far, and once it reaches the cap an exponential average with about that memory.
*/

// The age after one more frame: one more than the history's, at most historyFrames (a history of age 0 gives 1: the
// new frame alone).
float aoHistoryAge(float previousAge, int historyFrames);

// The new frame's share in the average at this age (at least 1).
float aoCurrentShare(float age);

// Whether a texel of last frame's history shows the point: its stored view depth (0 where it held the sky or nothing)
// within share of the depth the point had then.
bool aoHistoryTapKept(float expectedDepth, float storedDepth, float share);

// Where a pixel's point was on last frame's screen and its view depth then, from its pixel (without the jitter), its
// depth and its view depth now; not valid behind last frame's camera or off its screen.
struct AoPreviousPlace {
    Vec2 pixel;
    float viewDepth = 0.0f;
    bool valid = false;
};

AoPreviousPlace aoPreviousPlace(const Mat4& reprojection, Vec2 pixel, float depth, float viewDepth, Vec2 size);

// A bilinear tap's weight (tap 0 to 3: bit 0 the texel to the right, bit 1 the one below), from the fraction past the
// first texel's centre.
float aoTapWeight(Vec2 fraction, int tap);

// One texel of the history: the average (rgb the gathered light, a the visibility), the view depth (0 for the sky) and
// the age.
struct AoHistoryTexel {
    Vec4 light;
    float viewDepth = 0.0f;
    float age = 0.0f;
    float fastMean = 0.0f;     // anti-lag: the gathered light's luminance over about the last 3 frames
    float secondMoment = 0.0f; // and its square's average over the history, for the history's own noise
};

using AoHistoryImage = std::function<AoHistoryTexel(int x, int y)>;

// What last frame's history holds for a point: the light of the bilinear taps that show it, their weights made to add
// up to 1, and the age, each tap's times its bilinear weight, so a point only partly seen last frame trusts it less.
// Age 0 and no light where no tap shows it.
struct AoHistory {
    Vec4 light;
    float age = 0.0f;
    float fastMean = 0.0f;
    float secondMoment = 0.0f;
};

AoHistory readAoHistory(const AoHistoryImage& history, RenderSize size, const AoPreviousPlace& place, float share);

// The new frame blended into the history's light at the new age.
Vec4 accumulatedAo(const Vec4& current, const Vec4& historyLight, float age);

// The gathered light's luminance (Rec. 709 weights), the one number anti-lag watches.
float aoLuminance(const Vec3& light);

/*
* Anti-lag's test: the fast average (each frame weighing AO_ANTILAG_FAST_SHARE) strays from the slow one by more than
* deviations of its own spread. The history's noise is sigma^2 = secondMoment - slowMean^2, and an exponential average
* weighing each frame a wobbles by sigma sqrt(a / (2 - a)). The threshold is never below AO_ANTILAG_RELATIVE_FLOOR of the
* slow mean plus AO_ANTILAG_ABSOLUTE_FLOOR, so a near-black pixel, with no noise to measure, does not restart on rounding.
*/
bool aoLightChanged(float fastMean, float slowMean, float secondMoment, float deviations);

// The new frame's neighbourhood for the history clip: the mean of its values and of their squares, each channel on its
// own; not valid where it holds nothing.
struct AoClipBox {
    Vec4 mean;
    Vec4 meanOfSquares;
    bool valid = false;
};

// The 3 x 3 texels of the new frame round (x, y) that lie on screen and are not sky.
AoClipBox aoNeighbourhood(int x, int y, RenderSize size, const AoDepthImage& depthAt, const AoLightImage& currentAt);

// The history's light held within mean plus or minus deviations of the spread sqrt(meanOfSquares - mean^2), each
// channel on its own (a spread rounded below 0 is none); deviations 0 leaves it as it is.
Vec4 clipAoHistory(const Vec4& historyLight, const Vec4& mean, const Vec4& meanOfSquares, float deviations);

/*
* The new frame (rgb the gathered light, a the visibility) blended into the history, at the point's view depth, with the
* fast mean and second moment carried on. With anti-lag, a history at least AO_ANTILAG_LEAST_AGE frames old whose light
* has changed (aoLightChanged: the fast mean with this frame against the history's slow mean and noise before it, since
* the jump itself would swell the noise and hide it) restarts from the new frame alone, its noise estimate with it; a
* younger one follows quickly anyway. Otherwise, with a valid box, the history's light is clipped into it
* (settings.historyClip) before the blend; anti-lag looks at it unclipped, so its own test is unchanged.
*/
AoHistoryTexel accumulateAo(const Vec4& current, float viewDepth, const AoHistory& history, const AmbientOcclusionSettings& settings,
                            const AoClipBox& box = {});

/*
* The temporal pass's loop, as ambient_occlusion_temporal.comp runs it, at texel (x, y): the sky holds open, nothing
* gathered, no depth and no age; otherwise the pass's result there (current) blended with last frame's history where it
* shows the point, or alone without a usable history.
*
* Each texel's result is taken for its whole footprint, wherever the jitter put its sample this frame: the jitter cycles,
* so the average covers the footprint, as supersampling would. So the texel's centre is reprojected without taking the
* jitter off, and a still camera reads exactly its own texel every frame; taking it off (and putting last frame's back
* on) would read between texels each frame and blur the history a little more every time.
*
* With the pass's whole result (currentAt, whose texel (x, y) is current) and the history clip on, the history is
* clipped into its neighbourhood there (aoNeighbourhood); without it, nothing is clipped.
*/
AoHistoryTexel aoTemporalAt(int x, int y, const AoFrame& frame, const Mat4& reprojection, const AmbientOcclusionSettings& settings,
                            const AoDepthImage& depthAt, const Vec4& current, const AoHistoryImage& history, bool historyUsable,
                            const AoLightImage& currentAt = {});

// What last frame's history was made for: a history is only carried over to a frame of the same size gathering the
// same way (the light means something else with and without gathering) with last frame's camera known.
struct AoHistoryKey {
    RenderSize size;
    bool gathers = false;
};

bool aoHistoryUsable(const std::optional<AoHistoryKey>& last, const AoHistoryKey& now, bool previousCameraKnown);

// The pass record (std430, a storage buffer): the camera-relative view-projection's inverse (jitter included), the
// render size and its inverse, x the pixel footprint per metre and yz this frame's jitter in render pixels, then the
// radius, falloff, slices and steps, the frame (turning the noise; 0 while there is no anti-aliasing, whose blur wants it
// still), this frame's clip space (without the jitter) to last frame's, x the thickness, y 1 while gathering and z
// the brightest sample, and the history: x its frames, y its depth share, z 1 while accumulating, w 1 when last
// frame's history is usable (set by VulkanAmbientOcclusion, which knows), the denoiser: x its passes, y its plane share, and the history's guards: x 1 while
// anti-lag is on, y its deviations, z the history clip's deviations (0 off).
struct GpuAmbientOcclusionPass {
    float inverseViewProjection[16];
    float size[4];
    float lens[4];
    float options[4];
    std::int32_t frame[4];
    float reprojection[16];
    float gather[4];
    float history[4];
    float denoise[4];
    float antiLag[4];
};

static_assert(sizeof(GpuAmbientOcclusionPass) == 256, "GpuAmbientOcclusionPass must match ambient_occlusion.comp's Pass");

GpuAmbientOcclusionPass packAmbientOcclusionPass(const Mat4& inverseViewProjection, const Mat4& viewProjection, const Mat4& reprojection, Vec2 jitter,
                                                 RenderSize size, const AmbientOcclusionSettings& settings, std::uint32_t frame, bool gathers,
                                                 bool accumulates);
