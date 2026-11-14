#pragma once

#include "core/settings_visitor.h"
#include "math/mat4.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "render/gpu/upscaling.h"
#include "render/gpu/volumetric_fog_shared.h"
#include "scene/fog.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

/*
* Volumetric fog: light scattered by the air towards the eye, so beams and lamps light the fog round them and distant
* things fade into it. Along a view ray, what reaches the eye is the surface's light dimmed by the fog in between,
* T(d) = exp(-integral of extinction), plus the light the fog scatters into the ray on the way:
*
*   L_eye = T(d) L_surface + integral from 0 to d of T(s) extinction(s) albedo S(s) ds,
*
* S the light arriving at s from every light, each weighted by the phase function (how much of it the droplets turn
* towards the eye) and its shadow there: single scattering, the air lit once on the light's way to the eye.
*
* Rather than march every pixel's ray, the GPU fills a grid shaped like the view (froxels: frustum voxels, gridWidth x
* gridHeight across the screen, slices deep out to range), three compute passes a frame:
* - fog_light.comp: each froxel's extinction (the Fog's height fog) and its light (the sun with its cascades' shadow,
*   the point and spot lights its cluster lists with theirs, the light probes' light from all around), at a point that
*   moves round the froxel's middle each frame, blended with last frame's grid carried along to where the point was;
* - fog_integrate.comp: each column of froxels front to back, the light gathered and let through to the far side of
*   each slice (exact for constant fog in a slice: energy-conserving, Hillaire 2015);
* - fog_apply.comp: each pixel of the picture before the glass dimmed and lit by the grid at its depth, past the grid
*   the height fog's closed form lit by the sun and sky without shadows. Glass applies it to itself as it is drawn.
*
* Units: light values are irradiance (render/gpu/brdf.h), pixels pi times radiance, so a light of irradiance E adds
* pi p E per unit of optical depth, and the probes' irradiance from above and below gives the light from all around.
* This is the API-free part, mirrored by assets/shaders/volumetric_fog.glsl and checked against it by the GPU tests.
*/

struct FogSettings {
    // On every GPU but the Low preset's; needs a Fog in the frame to show anything.
    bool enabled = true;

    // How far the grid reaches (m, no further than the camera's far plane); beyond, the height fog's closed form.
    float range = 100.0f;

    // The froxels across and down the screen and in depth: about 16 pixels square at 2560 x 1440.
    int gridWidth = 160;
    int gridHeight = 90;
    int slices = 64;

    // How much of last frame's light each froxel keeps (0 none: noisy, and moving lights leave no trail).
    float historyWeight = 0.9f;

    // The light from all around read from the light probes (when the frame has them), else the flat ambient.
    bool probeLight = true;

    // Samples of the sun's shadow each pixel takes along the fog past the grid (0 to FOG_MAX_FAR_SHADOW_STEPS; 0 leaves
    // that fog sunlit throughout, as before): each carries an equal share of the light the fog there scatters
    // (fogFarSampleDistance), turned per pixel and frame for the anti-aliasing to average. Without them the mist between
    // the camera and a pillar hundreds of metres away, in its shadow, was lit as brightly as open air towards the sun
    // (the user's look at the forest, Nov 14 2026). Only the picture pass reads them; glass sees that fog unshadowed.
    int farShadowSteps = 16;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(FogSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a range, grid size, slice count, history weight or far shadow steps outside
// volumetric_fog_shared.h's limits.
void validateFog(const FogSettings& settings);

// The fog's extinction per metre at a height: the density at the base height, falling by e every scale height.
float fogDensity(const Fog& fog, float height);

/*
* The fog's optical depth (the integral of its extinction) along a ray from originHeight whose direction rises
* directionY per metre, from start to end metres along it: the closed form of the exponential, the density at start times
* the length times (1 - exp(-x)) / x for x the exponent's change (its series near 0, so a level ray gives density x
* length), the exponents capped at FOG_MAX_EXPONENT.
*/
float fogOpticalDepth(const Fog& fog, float originHeight, float directionY, float start, float end);

// A slice coordinate (0 at the camera, 1 at the range) as a view depth (m along the view), and back (past 1 beyond the range).
float fogSliceDepth(float slice, float range);
float fogDepthSlice(float depth, float range);

/*
* A camera-relative point's place in the grid: across the screen 0..1, down it 0..1 (rows from the top, as textures
* run), and its view depth's slice coordinate, through the camera's view-projection (camera-relative, without the
* jitter). Its clip w is the view depth.
*/
Vec3 fogVolumeCoordinate(const Vec3& relativePoint, const Mat4& viewProjection, float range);

// The camera-relative point at a place in the grid: along the ray through that screen place, at the slice's view depth.
Vec3 fogVolumePoint(const Vec3& coordinate, const Mat4& inverseViewProjection, const Vec3& viewForward, float range);

// This frame's offset of the froxels' sample points from their middles, in froxels, each within half of one: the Halton
// (2, 3, 5) point of the frame's place in a cycle of fogJitterFrames (counted from 1, since point 0 is the corner).
inline constexpr std::uint64_t fogJitterFrames = FOG_JITTER_FRAMES;
Vec3 fogJitter(std::uint64_t frame);

// A froxel's place in the grid: across, down and in depth, each from 0.
using FogFroxel = std::array<int, 3>;

/*
* A froxel's own turn of the frames' offsets, each 0..1: the PCG3D hash of its place (Jarzynski and Olano 2020), its top
* 24 bits as a share. Integer arithmetic, so the shader's is the same to the bit.
*/
Vec3 fogFroxelTurn(const FogFroxel& froxel);

/*
* This frame's offset of one froxel's sample point from its middle: the frame's jitter turned by the froxel's own turn
* and wrapped round into -0.5..0.5 (a Cranley-Patterson rotation). Each froxel still visits its cycle's evenly spread
* points, but neighbours visit them in different places at once, so what the history leaves is fine noise rather than
* one pattern moving over the whole grid (which shimmered).
*/
Vec3 fogFroxelJitter(const Vec3& frameJitter, const FogFroxel& froxel);

// A froxel's place in the grid, 0..1 each way, at an offset from its middle in froxels, held inside the grid.
Vec3 fogFroxelCoordinate(const FogFroxel& froxel, const FogFroxel& size, const Vec3& offset);

/*
* Where a froxel's history is read: its middle's point in last frame's grid (previousViewProjection takes this frame's
* camera-relative points). With the camera still, exactly its own texel's middle. Read at the jittered sample point
* instead, the filter mixed in the neighbours with weights changing every frame, and sunbeams' edges shimmered.
*/
Vec3 fogHistoryCoordinate(const FogFroxel& froxel, const FogFroxel& size, const Mat4& inverseViewProjection, const Mat4& previousViewProjection,
                          const Vec3& viewForward, float range);

// The light a light of this irradiance scatters towards the eye per unit of optical depth (in pixel units): pi times the
// phase function (Henyey-Greenstein, anisotropy g) at the angle between the view direction and the way to the light.
Vec3 fogInScattering(const Vec3& irradiance, const Vec3& toLight, const Vec3& viewDirection, float anisotropy);

// The light from all around, from the irradiance from above and below (the light probes'): a uniform sky of radiance L
// gives pi L each way, and so pi L.
Vec3 fogAmbient(const Vec3& irradianceUp, const Vec3& irradianceDown);

// The fog along a ray so far: the light it gathered and the share of what lies behind it lets through.
struct FogIntegration {
    Vec3 light;
    float transmittance = 1.0f;
};

/*
* Add one slab of constant fog: scattered is the light its froxel scatters towards the eye per unit of optical depth
* (albedo times the lights), extinction per metre over length metres. The slab adds scattered x (1 - exp(-depth)) of
* light, dimmed by what lies before it, and lets exp(-depth) through: exact, however thick the slab.
*/
void fogIntegrateSlice(FogIntegration& sum, const Vec3& scattered, float extinction, float length);

// A stretch of fog followed by another behind it.
FogIntegration fogFollow(const FogIntegration& front, const FogIntegration& behind);

// A surface's light seen through the fog in front of it.
Vec3 fogComposite(const Vec3& colour, const FogIntegration& fog);

/*
* fogComposite undone: the surface's own light from a picture seen through the fog in front of it, for light gathered
* from the picture (ambient occlusion's bounce), which should be what the surface sends, not the fog's glow between it
* and the camera. Never below black (a blurred picture can be darker than the fog at one point), and the dimming undone
* only down to FOG_REMOVAL_MIN_TRANSMITTANCE.
*/
Vec3 fogRemoved(const Vec3& seen, const FogIntegration& fog);

/*
* What glass adds through the fog in front of it, drawn over a picture already fogged up to the surface behind (which
* holds the same fog's light): its own light dimmed by the fog, plus the fog's light for the share the glass does not
* let through (per colour), so glass over the picture gives the glass and the surface fogged in turn.
*/
Vec3 fogOverGlass(const Vec3& added, const Vec3& transmittance, const FogIntegration& fog);

/*
* Where the integrated grid is read for a slice coordinate: texel k holds the fog to the far side of slice k, so a
* coordinate is read half a texel earlier (z, clamped to the texels' middles), and inside the first half slice the
* reading fades towards no fog by share (0 at the camera, 1 from half a slice on).
*/
struct FogSliceRead {
    float z = 0.0f;
    float share = 0.0f;
};

FogSliceRead fogSliceRead(float slice, int slices);

// Fog as a medium: the light it scatters towards the eye and its extinction, each per metre.
struct FogMedium {
    Vec3 light;
    float extinction = 0.0f;
};

// One slice's medium, from the integrated grid to its near side (before) and its far side (after) and its depth.
FogMedium fogSliceMedium(const FogIntegration& before, const FogIntegration& after, float length);

// a and b blended: b's share t.
FogMedium mixFogMedium(const FogMedium& a, const FogMedium& b, float t);

// The fog through length metres of a medium (nearly clear: its light in proportion to the length).
FogIntegration fogThroughMedium(const FogMedium& medium, float length);

/*
* How the fog to a surface is read without the slice the surface lies in, whose light was sampled at a jittered point
* that may lie behind the surface (sunlit air past a wall streaked the wall): the grid read as usual up to one slice
* before the surface (front, a slice coordinate, all of it in slices wholly in front), then over the last slice's
* depth (stretch, metres) a medium sliding from slice last - 1's to slice last's as the surface moves through its
* slice (mix), last being the slice before the surface's: continuous across slice boundaries (taking one slice's
* medium jumped there and drew lines down walls seen at an angle) and exact when the slices are alike. Not extended
* inside the first slice or past the grid: the grid as fogSliceRead reads it.
*/
struct FogSurfaceRead {
    bool extended = false;
    int last = 0;
    float mix = 0.0f;
    float front = 0.0f;
    float stretch = 0.0f;
};

FogSurfaceRead fogSurfaceRead(float slice, int slices, float range);

// How much of last frame's grid a froxel keeps: the weight where its point was inside last frame's grid, else nothing.
float fogHistoryWeight(const Vec3& previousCoordinate, float weight);

/*
* The integration pass for one column, mirrored: each froxel's (scattered light, extinction) front to back, each slice
* as long as its depth step times stretch (the ray's length per metre of view depth), the fog to the far side of each.
*/
std::vector<FogIntegration> integrateFogColumn(std::span<const Vec4> lighting, float range, float stretch);

// The fog from start to end metres along a ray past the grid: the height fog's closed form, lit by one source (the light
// scattered per unit of optical depth); nothing when end is not past start.
FogIntegration fogBeyondGrid(const Fog& fog, const Vec3& source, float originHeight, float directionY, float start, float end);

/*
* Where along a ray past the grid (from start to end metres) share (0..1) of the light the fog there scatters towards the
* eye has been gathered: the light from a stretch is its own 1 - exp(-depth) through the fog in front of it, so in all
* 1 - exp(-total), and share of it is reached at the depth -ln(1 - share (1 - exp(-total))), turned into metres by the
* closed form's inverse (the density at start k, the exponent's change per metre a: -ln(1 - a depth / k) / a, its
* series near a = 0). Equal shares give samples of equal weight, so the mean of the sun's shadow at them is the share
* of that light which is sunlit. Without fog the samples spread evenly; start when end is not past it.
*/
float fogFarSampleDistance(const Fog& fog, float originHeight, float directionY, float start, float end, float share);

/*
* The frame's fog for every pass that reads it (std430, scene set 1 binding 27): the grid's camera (camera-relative,
* without the jitter), its inverse and last frame's (taking this frame's camera-relative points), the grid (froxels
* across, down, slices, range), the medium (density, base height, scale height, anisotropy), the albedo with the
* farthest distance in w, the options (x 1 when on, y the history weight (0 without a usable history), z 1 reading the
* probes) and the frame's jitter.
*/
struct GpuFog {
    float viewProjection[16];
    float inverseViewProjection[16];
    float previousViewProjection[16];
    float grid[4];
    float medium[4];
    float albedo[4];
    float options[4];
    float jitter[4];
};

static_assert(sizeof(GpuFog) == 272, "GpuFog must match scene_fog.glsl's Fog");

// The frame's record: off without a fog or with the setting off; the range no further than the camera's far plane; the
// air lit by the light probes only when probesRead (the frame has them), since without them their reading falls back to
// the open sky's light, which lit the air in closed rooms as outdoors (the sky visibility grid lights it instead).
GpuFog packFog(const Fog* fog, const FogSettings& settings, const Mat4& viewProjection, const Mat4& previousViewProjection, bool historyUsable,
               bool probesRead, std::uint64_t frame, float cameraFar);

// The picture pass's push constants (fog_apply.comp): the camera-relative view-projection's inverse as the frame was
// drawn (jitter included), and the render size, the far shadow's steps and the frame (turning each pixel's samples).
struct GpuFogPicturePass {
    float inverseViewProjection[16];
    float size[4];
};

static_assert(sizeof(GpuFogPicturePass) == 80, "GpuFogPicturePass must match fog_apply.comp's Pass");

GpuFogPicturePass packFogPicturePass(const Mat4& inverseViewProjection, RenderSize size, int farShadowSteps, int frame);
