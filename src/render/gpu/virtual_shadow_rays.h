#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/virtual_shadows.h"
#include "render/gpu/virtual_shadows_shared.h"

#include <functional>
#include <optional>
#include <vector>

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
* A sample reads the first level, from the point's own, where it lies within VIRTUAL_SHADOW_RAY_TEXELS of the point
* across (4d-ii): the point and the pixel's coarser levels (VirtualShadowSettings::rayLevels of them) each marked that
* far round it, so every sample lies on a marked page and a ray climbing to a caster far above reads coarse texels as
* wide as its spread (a pillar's edge 100 m up: a 47 cm penumbra). A ray's end reaches at most 12 x 2^rayLevels of the
* point's level's texels, as Unreal clamps its penumbra. A sample whose page is not drawn reads the next coarser drawn
* level. Each level has its own depth scale, so the march works in metres along the light from the point. This file
* is the maths, mirrored by assets/shaders/virtual_shadow_rays.glsl; the main pass's march reads the pool
* (scene_surface.glsl, virtualShadowSun) as virtualShadowRayVisibility does here.
*/

inline constexpr float virtualShadowRayTexels = static_cast<float>(VIRTUAL_SHADOW_RAY_TEXELS);

/*
* A ray's point on the sun's disc (unit radius): a Fibonacci (Vogel) spiral, every ray an equal share of the disc's area,
* turned by turn (in whole turns, the pixel's noise) so neighbouring pixels sample it differently and the temporal pass
* averages them.
*/
Vec2 virtualShadowRayDisc(int ray, int rays, float turn);

/*
* How far up its ray a sample lies, as a share of the ray's height: the last at its end (1), the rest by
* ((sample + jitter) / samples)^p, nearer together near the point, where contact shadows are sharp. p is 2, or levelsUp
* when more: a ray reaching levelsUp levels coarser is 2^levelsUp times longer, and half its samples stay within the
* point's own level's reach (0.5^p = 2^-levelsUp), so contact shadows keep their samples. jitter in [0, 1).
*/
float virtualShadowSampleShare(int sample, int samples, float jitter, int levelsUp);

/*
* A ray's height along the light (metres) from a point at startDepth of its level: as high as its widest sideways reach
* (a disc point at the rim) stays within VIRTUAL_SHADOW_RAY_TEXELS of the level levelsUp coarser (12 x 2^levelsUp of
* this one's texels), and no higher than this level's depth range holds sunwards (depth 1, the caster reach included).
* inverseTexel and depthScale as VirtualShadowPlacement's.
*/
float virtualShadowRayHeight(float tanAngularRadius, float inverseTexel, float depthScale, float startDepth, int levelsUp);

// How many levels coarser than the point's a sample offset this many of the point's level's texels reads: the first
// where it lies within VIRTUAL_SHADOW_RAY_TEXELS (each level's texels twice the last's; by doubling, exact), at most
// levelsUp.
int virtualShadowSampleLevel(float offsetTexels, int levelsUp);

// A stored depth of a level as a height along the light above the point (metres; the point's depth and the level's
// scale there), so samples of different levels march together.
float virtualShadowStoredHeight(float stored, float pointDepth, float depthScale);

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
* A ray's march so far, in heights along the light (metres above the point, larger is sunwards; the march's rules hold
* in any such unit). under: the last sample was behind a stored surface; that surface's height at the ray height it was
* seen at, and its slope (surface height per ray height).
*/
struct VirtualShadowMarch {
    bool under = false;
    bool hit = false;
    float surfaceDepth = 0.0f;
    float surfaceRayDepth = 0.0f;
    float surfaceSlope = 0.0f;
    float lastRayDepth = 0.0f; // the ray's height at the last sample (0: the point)
    bool checking = false;     // the ray crossed checkDepth between the last two samples: read the surface there
    float checkDepth = 0.0f;

    bool operator==(const VirtualShadowMarch&) const = default;
};

/*
* One sample: the ray at rayDepth, the stored surface at stored there. tolerance (the sample's level's bias) allows for
* texels sampled beside the ray; slopeCap (VirtualShadowSettings::extrapolateSlope times tan(a): surface height per ray
* height) bounds how a surface may continue between samples and behind a higher one.
* - Under a stored surface: the first time, it becomes the surface; if it is the same surface carried on (within the
*   tolerance and the slope), its slope is updated; if a higher one hides it, it is carried on behind (with a cap above
*   0; else forgotten), and the ray rising to it is blocked; a lower one: the old surface ended, the lower one taken.
* - Out from under a surface, having risen to its height carried on here: through it, or out beside it first? Out in
*   the open past a surface higher than the ray was at the last sample: through it, or past its edge? Either way the
*   ray crossed that height between the two samples; the march asks (checking) for the surface where it did.
*/
VirtualShadowMarch virtualShadowMarchStep(const VirtualShadowMarch& march, float rayDepth, float stored, float tolerance, float slopeCap);

/*
* The refinement read (as screen-space ray marches refine a hit): where the ray crossed the asked height, clamped
* between the last two samples' ray heights; there, blocked if the surface reaches that height (within its level's
* tolerance), or if no page holds it (the step's guess). One extra read a crossing, not a sample.
*/
float virtualShadowCrossingDepth(const VirtualShadowMarch& march, float previousRayDepth, float rayDepth);
VirtualShadowMarch virtualShadowMarchCheck(const VirtualShadowMarch& march, std::optional<float> storedAtCrossing, float tolerance);

// After the last sample: blocked if it hit, or if it ends still under a surface (a taller caster).
bool virtualShadowRayBlocked(const VirtualShadowMarch& march);

// The point in one of the levels a ray reads: its texel and (unbiased) depth there, the level's inverse texel and depth
// scale, and its bias in that level's depth (virtualShadowBiasDepth).
struct VirtualShadowRayLevel {
    Vec3 point;
    float inverseTexel = 0.0f;
    float depthScale = 0.0f;
    float biasDepth = 0.0f;
};

/*
* The share of the sun's disc a point sees: the main pass's march (virtualShadowSun) through levels, the point's own
* first and each next one coarser, over depths given by storedAt (a level's index in levels and its texel to the stored
* reversed depth; nullopt where no page is drawn: the next coarser level is read, past all the sample is skipped). The
* ray starts the point's own level's bias above it. turn and jitter: the pixel's noise.
*/
float virtualShadowRayVisibility(const std::vector<VirtualShadowRayLevel>& levels, float tanAngularRadius, const VirtualShadowSettings& settings,
                                 float turn, float jitter, const std::function<std::optional<float>(int, float, float)>& storedAt);
