#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/virtual_shadows.h"
#include "render/gpu/virtual_shadows_shared.h"

#include <functional>
#include <optional>

/*
* Soft shadows through the sun's virtual shadow pages by shadow-map ray tracing (SMRT, Unreal's; ROADMAP phase 21 4d-i;
* research and choices: docs/research/VIRTUALSHADOWMAPS.md "Soft shadows through the pages"). From a point the sun is a
* disc of angular radius a, so a ray towards one of its points u (|u| <= 1) climbs a height h along the light while it
* moves h tan(a) u across it. A level of the map is a straight map of (across, along) to (texels, depth), so the ray is a
* straight segment of texels and depths, marched in a few samples against the depths stored there. The share of rays
* that get through is the share of the disc the point sees: each ray finds its own blocker (a canopy over a canopy, sun
* flecks through gaps), where an averaged blocker height would smear them.
*
* The map holds only the first surface sunwards, so the march treats surfaces as thin shells: a ray that passes under a
* surface is blocked only by rising through it, not by being under it (a small leaf high up leaves the point below it
* partly lit, as the sun's disc is wider than the leaf). Where a higher surface hides the one the ray is under, that one
* is carried on behind it (gap filling: Unreal's extrapolation behind occluders), its slope capped, so the ray still
* meets it. A ray that ends still under a surface is blocked: taller casters cast, their penumbra capped.
*
* Rays are clamped to VIRTUAL_SHADOW_RAY_TEXELS of the point's level across, as Unreal clamps its penumbra, so every
* sample lies on the pages the point marked (at most 2 x 2) and the penumbra is at most that wide. This file is the
* maths, mirrored by assets/shaders/virtual_shadow_rays.glsl; the main pass's march reads the pool
* (scene_surface.glsl, virtualShadowSun) as virtualShadowRayVisibility does here.
*/

inline constexpr float virtualShadowRayTexels = static_cast<float>(VIRTUAL_SHADOW_RAY_TEXELS);

/*
* A ray's point on the sun's disc (unit radius): a Fibonacci (Vogel) spiral, every ray an equal share of the disc's area,
* turned by turn (in whole turns, the pixel's noise) so neighbouring pixels sample it differently and the temporal pass
* averages them.
*/
Vec2 virtualShadowRayDisc(int ray, int rays, float turn);

// How far up its ray a sample lies, as a share of the ray's height: the last at its end (1), the rest by the square of
// (sample + jitter) / samples, nearer together near the point, where contact shadows are sharp. jitter in [0, 1).
float virtualShadowSampleShare(int sample, int samples, float jitter);

/*
* A ray's height along the light (metres) from a point at startDepth of a level: as high as its widest sideways reach
* (a disc point at the rim) stays within VIRTUAL_SHADOW_RAY_TEXELS, and no higher than the level's depth range holds
* sunwards (depth 1, the caster reach included). inverseTexel and depthScale as VirtualShadowPlacement's.
*/
float virtualShadowRayHeight(float tanAngularRadius, float inverseTexel, float depthScale, float startDepth);

// A sample height metres up a ray towards disc point disc, from start (the point's texel x, y and biased depth z).
Vec3 virtualShadowRaySample(const Vec3& start, const Vec2& disc, float tanAngularRadius, float inverseTexel, float depthScale, float height);

/*
* The pages a point's samples can land on: the first page of the square VIRTUAL_SHADOW_RAY_TEXELS each way round its
* texel (the marked square, virtualShadowMarkedPages), and which of its at most 2 x 2 pages a sample's page is: 0 the
* first, 1 one across, 2 one down, 3 both. The main pass reads the four entries once a point.
*/
VirtualShadowPage virtualShadowFootprintFirst(float texelX, float texelY);
int virtualShadowFootprintIndex(const VirtualShadowPage& page, const VirtualShadowPage& first);

/*
* A ray's march so far, in the level's reversed depths (larger is sunwards). under: the last sample was behind a stored
* surface; that surface's depth at the ray depth it was seen at, and its slope (stored depth per ray depth).
*/
struct VirtualShadowMarch {
    bool under = false;
    bool hit = false;
    float surfaceDepth = 0.0f;
    float surfaceRayDepth = 0.0f;
    float surfaceSlope = 0.0f;

    bool operator==(const VirtualShadowMarch&) const = default;
};

/*
* One sample: the ray at rayDepth, the pool storing stored there. tolerance (a reversed depth step, the point's bias)
* allows for texels sampled beside the ray; slopeCap (VirtualShadowSettings::extrapolateSlope times tan(a): stored
* depth per ray depth) bounds how a surface may continue between samples and behind a higher one.
* - Under a stored surface: the first time, it becomes the surface; if it is the same surface carried on (within the
*   tolerance and the slope), its slope is updated; if a higher one hides it, it is carried on behind (with a cap above
*   0; else forgotten), and the ray rising to it is blocked; a lower one: the old surface ended, the lower one taken.
* - Out from under a surface: blocked if the ray rose to that surface carried on here (it went through it, or through
*   the gap between two samples); not if it slipped out sideways beneath it.
*/
VirtualShadowMarch virtualShadowMarchStep(const VirtualShadowMarch& march, float rayDepth, float stored, float tolerance, float slopeCap);

// After the last sample: blocked if it hit, or if it ends still under a surface (a taller caster).
bool virtualShadowRayBlocked(const VirtualShadowMarch& march);

/*
* The share of the sun's disc a point sees: the main pass's march (virtualShadowSun) over depths given by storedAt (a
* level's texel to its stored reversed depth; nullopt where no page is drawn, which the sample skips). start: the
* point's texel and its depth lifted by the bias (which is also the tolerance); turn and jitter the pixel's noise.
*/
float virtualShadowRayVisibility(const Vec3& start, float biasDepth, float tanAngularRadius, float inverseTexel, float depthScale,
                                 const VirtualShadowSettings& settings, float turn, float jitter,
                                 const std::function<std::optional<float>(float, float)>& storedAt);
