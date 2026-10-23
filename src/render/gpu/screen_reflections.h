#pragma once

#include "core/settings_visitor.h"
#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/reflection_probe.h"
#include "render/gpu/screen_reflections_shared.h"
#include "render/gpu/upscaling.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/*
* Screen-space reflections (SSR). Each glossy pixel follows its mirror ray through the depth image: where the ray
* passes behind the surface the screen shows, it has hit that surface, and the reflection is that pixel's colour in
* last frame's finished picture. The ray marches through a pyramid of the depth image (Hi-Z), each level keeping the
* nearest depth of the 2x2 texels below it, so empty space is crossed in a few long steps. Rougher surfaces read a
* blurrier level of a pyramid of the picture, as wide as their reflected cone where it lands.
*
* The main pass keeps adding the light probes' stand-in for the reflection, and writes it out with the pixel's normal,
* roughness and specular albedo; the reflection pass then swaps the stand-in for the reflection, as far as it trusts the
* hit (the confidence): rays that leave the screen, go far, hit a surface from behind, or leave a rough surface fade
* back to the stand-in.
*
* This is the API-free part, mirrored by assets/shaders/screen_reflections.glsl and checked against it by the GPU tests.
*/

struct ReflectionSettings {
    // Off draws no reflection images and runs no pass: the light probes' stand-in alone.
    bool enabled = true;

    // The two sources, each off on its own for comparing: what the screen shows, and the reflection probe round the
    // camera (render/gpu/reflection_probe.h) for what it does not.
    bool screenSpace = true;
    bool probe = true;

    // Up to this roughness the reflection is trusted fully; from maxRoughness on not at all (the stand-in, right for
    // blurry reflections, takes over), smoothly between.
    float sharpRoughness = 0.3f;
    float maxRoughness = 0.6f;

    // How far behind the surface the screen shows (view distance, metres) a ray may be and still count as hitting it:
    // the depth image holds only front faces, so a ray passing behind a thin pole would otherwise hit it.
    float thickness = 0.3f;

    // How far a ray goes (metres); hits fade out over its last fifth. Hits far away are as good as near ones on screen.
    float reach = 1000.0f;

    // The most steps a ray takes through the depth pyramid before giving up.
    int maxSteps = 64;

    // The share of the screen's half-width near each edge over which hits fade out, so a reflection leaving the screen
    // does not end in a hard line.
    float edgeFade = 0.1f;

    // The reflection probe's faces, texels a side (a power of two, 16 to 1024): sharper reflections of what is off
    // screen, each doubling costing four times the capture.
    int probeSize = 256;

    // How far the probe's capture sees (metres); beyond it the sky.
    float probeFar = 300.0f;

    // On GPUs with ray queries: each glossy pixel's mirror ray traced through the scene, so what it hits is exact,
    // lit with the probe's picture where the probe saw it and as the light probes' rays light their hits elsewhere
    // (matte), in place of the probe's search. Off by default, as every ray-traced feature; ignored without ray queries.
    bool rayTraced = false;

    // A debug view: each pixel's confidence in its reflection (white: screen-space reflections, blue: the probe,
    // black: the stand-in).
    bool showConfidence = false;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(ReflectionSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for roughness limits out of order or outside 0..1, a negative thickness, no reach, no
// steps, an edge fade outside 0..1, a probe size that is not a power of two from 16 to 1024 or a probe reaching no
// further than its near plane.
void validateReflections(const ReflectionSettings& settings);

// The depth pyramid's levels for a depth image of this size: a full mip chain, at most SSR_MAX_PYRAMID_LEVELS.
std::uint32_t depthPyramidLevels(RenderSize size);

// A level's size, as the GPU sizes mips: each side halved per level, rounded down, at least 1.
RenderSize pyramidLevelSize(RenderSize size, std::uint32_t level);

// The last texel of the finer level (fineSize across) that texel coarse of the coarser level covers: 2 coarse + 1, and
// at the far edge of an odd finer level also the texel left over (2 coarse + 2), so every texel has a cell above it.
int pyramidLastFine(int coarse, int coarseSize, int fineSize);

// The cell of a level-0 texel at this level (levelSize across): the texel halved level times, the last cell also
// holding the texels left over at the far edge.
int pyramidCell(int texel, std::uint32_t level, int levelSize);

// The depth pyramid on the CPU, as depth_pyramid.comp builds it: level 0 the depth image, each texel above the nearest
// (in reversed-Z the largest) depth of the texels it covers.
struct DepthPyramid {
    std::vector<RenderSize> sizes;
    std::vector<std::vector<float>> levels;

    // The nearest depth in the cell at (x, y) of this level.
    float nearest(std::uint32_t level, int x, int y) const;
};

DepthPyramid buildDepthPyramid(std::span<const float> depths, RenderSize size);

// The nearer of two depths (reversed-Z: the larger), the pyramid's reduction.
float nearerDepth(float a, float b);

// A point on screen from a pixel (counted from the top-left corner) and its depth, measured from the camera, through the
// inverse of the camera-relative view-projection that drew it.
Vec3 positionAtPixel(const Mat4& inverseViewProjection, Vec2 pixel, float depth, Vec2 size);

// A pixel's distance along the view (clip w) at this depth: thickness is measured in it.
float viewDistanceAtPixel(const Mat4& inverseViewProjection, Vec2 pixel, float depth, Vec2 size);

// The world size of one pixel per metre of view distance, from the camera-relative view-projection: its second row is
// the lens's vertical scale times the camera's up, so its length is that scale.
float pixelFootprint(const Mat4& viewProjection, float height);

/*
* A ray on screen: in pixels (x, y from the top-left corner) and depth, all three straight lines in t, since a straight
* line in space stays straight on screen and reversed-Z depth is linear in 1 / w, as screen x and y are.
*/
struct ScreenRay {
    Vec3 start; // where t = 0: the pixel and depth the ray leaves from
    Vec3 step;  // the change per unit of t
    float end = 0.0f; // the last t on screen, in the depth range and within the reach
};

// The ray from a point measured from the camera along a unit direction, at most reach metres, onto a screen of this
// size through the camera-relative view-projection; cut where it would pass the camera's near side, leave the screen
// or the depth range.
ScreenRay screenRay(const Mat4& viewProjection, const Vec3& position, const Vec3& direction, float reach, Vec2 size);

// How much t takes the ray out of its square cell (cellSize texels a side) and SSR_CELL_CROSSING texels into the next.
float cellExit(Vec2 position, Vec2 step, float cellSize);

// Where a ray marching through the depth pyramid has got to: its point on screen, its t, and the pyramid level it is
// looking at; a level below 0 means it has hit.
struct HizTrace {
    Vec3 position;
    float t = 0.0f;
    int level = 0;
};

/*
* One step through the pyramid, given the nearest depth in the position's cell at its level. In front of everything in
* the cell: on to where the ray reaches that depth, a level finer, if that is within the cell, else out of the cell
* and a level coarser (at most topLevel). At or behind the cell's nearest depth: a level finer without moving, since
* something in the cell may hide the ray. Below level 0 it has hit the texel it is in.
*/
HizTrace hizStep(HizTrace trace, const ScreenRay& ray, float cellNearest, int topLevel);

// The ray passed behind a surface by more than the thickness: it goes on, out of its texel at level 0.
HizTrace passTexel(HizTrace trace, const ScreenRay& ray);

// Whether a ray this far along the view counts as hitting a surface this far: no more than thickness behind it.
bool withinThickness(float rayDistance, float surfaceDistance, float thickness);

// What a march found: whether it hit, and where on screen (pixel and depth) and at which t.
struct ScreenHit {
    Vec3 position;
    float t = 0.0f;
};

// The whole march, as reflections.comp runs it: the ray's start texel left first, then hizStep until a hit within the
// thickness, the end of the ray or maxSteps steps.
std::optional<ScreenHit> traceScreenRay(const DepthPyramid& pyramid, const ScreenRay& ray, const Mat4& inverseViewProjection,
                                        float thickness, int maxSteps);

// The things the confidence weighs, apart from the settings.
struct ReflectionHit {
    float roughness = 0.0f;
    Vec2 pixel;              // where it hit on screen
    float travelled = 0.0f;  // metres from the surface to the hit
    float facing = -1.0f;    // the cosine between the hit surface's normal and the ray: negative from the front
    float towardsCamera = 0.0f; // the cosine between the ray and the way from its surface back to the camera
};

// How far the reflection is trusted over the stand-in, 0..1: the roughness fade, times the screen edge fade, times the
// fade over the reach's end, times the front face test, times the fade for rays heading back towards the camera.
float reflectionConfidence(const ReflectionHit& hit, Vec2 size, const ReflectionSettings& settings);

// The colour pyramid's level a reflected cone this rough reads after travelling this many pixels: as wide as the cone,
// its width doubling per level; 0 (the sharp picture) for a mirror.
float reflectionConeLevel(float roughness, float travelledPixels);

// The scene with the stand-in swapped for the reflection by the confidence: scene + confidence (specularAlbedo hit -
// standIn), never below 0; the scene exactly at confidence 0.
Vec3 reflectedScene(const Vec3& scene, const Vec3& standIn, const Vec3& specularAlbedo, const Vec3& hitLight, float confidence);

// What this frame's reflection pass has to work with besides the settings: the depth pyramid's levels, whether
// last frame's picture may be reflected (screen-space reflections run), and the probe, if one was captured: its
// prefiltered levels and where it was taken, measured from this frame's camera.
struct ReflectionPassFrame {
    std::uint32_t pyramidLevels = 1;
    bool screenSpace = false;
    std::uint32_t probeLevels = 0; // none: no probe
    Vec3 probeOrigin;
    std::uint32_t probeSize = 0;
    std::uint32_t probeDistanceLevels = 0;
    Vec3 background; // the light behind everything, for ray-traced reflections that escape to the sky
};

/*
* The reflection pass's numbers (reflections.comp's storage buffer, std430): the camera-relative view-projection that
* drew the frame (jitter included) and its inverse, the reprojection to last frame's picture (render/gpu/taa.h), the
* render size, the settings, the pyramid's top level, the pixel footprint, this frame's jitter, and the probe.
*/
struct GpuReflectionPass {
    float viewProjection[16];
    float inverseViewProjection[16];
    float reprojection[16];
    float size[4];        // xy: the render size in pixels, zw: their inverses
    float options[4];     // x: sharp roughness, y: max roughness, z: thickness, w: reach
    float limits[4];      // x: max steps, y: the pyramid's top level, z: edge fade, w: 1 to show the confidence
    float lens[4];        // x: pixel footprint per metre, yz: this frame's jitter in render pixels, w: unused
    float probe[4];       // x: 1 when the probe is read, y: its prefiltered levels, z: 1 when screen-space reflections run, w: its far plane
    float probeOrigin[4]; // xyz: where the probe was taken, measured from the camera
    float probeCube[4];   // x: the probe's faces in texels, y: its distance pyramid's levels
    float background[4];  // rgb: the light behind everything (ray-traced reflections that escape)
};

static_assert(sizeof(GpuReflectionPass) == 320, "GpuReflectionPass must match reflections.comp's buffer");

GpuReflectionPass packReflectionPass(const Mat4& viewProjection, const Mat4& inverseViewProjection, const Mat4& reprojection,
                                     RenderSize render, Vec2 jitter, const ReflectionSettings& settings, const ReflectionPassFrame& frame);

// The pyramid passes' push constants: the level written (its size) and the level read (its size).
struct GpuPyramidPass {
    std::int32_t target[4]; // xy: the level's size, z: the level, w: unused
    std::int32_t source[4]; // xy: the level below's size (the depth image's for level 0), zw: unused
};

static_assert(sizeof(GpuPyramidPass) == 32, "GpuPyramidPass must match depth_pyramid.comp's and colour_pyramid.comp's push constants");

GpuPyramidPass packPyramidPass(RenderSize target, std::uint32_t level, RenderSize source);
