#pragma once

#include "core/settings_visitor.h"
#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/ambient_occlusion_shared.h"
#include "render/gpu/upscaling.h"

#include <cstdint>
#include <functional>

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

    // A crease's walls bouncing light back in, more for bright ones (Jimenez 2016's fit).
    bool multipleBounces = true;

    // The reflection's stand-in dimmed in creases too (Lagarde 2014's fit).
    bool specularOcclusion = true;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(AmbientOcclusionSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a radius not above 0, a falloff outside (0, 1], or slices or steps outside 1 up to
// AO_MAX_SLICES and AO_MAX_STEPS.
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

// Step index of count's distance along a slice (pixels): spread quadratically (dense near the point, where creases are),
// staggered by noise (0..1), never under AO_MIN_SAMPLE_PIXELS.
float aoSampleDistance(int step, int count, float noise, float screenRadius);

// The visibility seen by a matte surface of this albedo, light bouncing between the crease's walls (never darker).
Vec3 multiBounceOcclusion(float visibility, const Vec3& albedo);

// How much of the reflection's stand-in a crease lets through, from the view's cosine, the visibility and the roughness.
float specularOcclusion(float nDotV, float visibility, float roughness);

// A neighbour's weight in the blur: 1 at the pixel's own view distance, falling to 0 at AO_BLUR_DEPTH_SHARE of it away.
float aoBlurWeight(float centreDistance, float neighbourDistance);

// The depth image as the pass reads it (reversed-Z), at a texel inside it.
using AoDepthImage = std::function<float(int x, int y)>;

// What the pass knows of the frame: the camera-relative view-projection's inverse, the render size, and a pixel's
// footprint per metre of view distance.
struct AoFrame {
    Mat4 inverseViewProjection;
    RenderSize size;
    float footprint = 0.0f;
};

/*
* The pass's loop, as ambient_occlusion.comp runs it, at texel (x, y): the visibility, 1 for the sky and for points
* whose radius is under AO_MIN_SAMPLE_PIXELS on screen. sliceNoise turns the slices, stepNoise staggers the steps (0..1).
* Samples off screen or on the sky count as open.
*/
float ambientOcclusionAt(int x, int y, const AoFrame& frame, const AmbientOcclusionSettings& settings, float sliceNoise, float stepNoise,
                         const AoDepthImage& depthAt);

// The pass record (std430, a storage buffer): the camera-relative view-projection's inverse (jitter included), the
// render size and its inverse, x the pixel footprint per metre, then the radius, falloff, slices and steps, and the frame
// (turning the noise; 0 while there is no anti-aliasing, whose blur wants it still).
struct GpuAmbientOcclusionPass {
    float inverseViewProjection[16];
    float size[4];
    float lens[4];
    float options[4];
    std::int32_t frame[4];
};

static_assert(sizeof(GpuAmbientOcclusionPass) == 128, "GpuAmbientOcclusionPass must match ambient_occlusion.comp's Pass");

GpuAmbientOcclusionPass packAmbientOcclusionPass(const Mat4& inverseViewProjection, const Mat4& viewProjection, RenderSize size,
                                                 const AmbientOcclusionSettings& settings, std::uint32_t frame);
