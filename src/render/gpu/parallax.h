#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "render/gpu/parallax_shared.h"

#include <functional>

/*
* Parallax occlusion mapping (Tatarchuk 2006): a height map makes a flat surface seem to stand out of its polygon. A
* normal map only tilts the light; here each pixel follows its view ray down into the surface, as if the height map were
* a little landscape under the polygon (white the top, at the polygon, black the deepest, depth below it), until the ray
* first drops below the ground, and reads every texture there instead of where the flat polygon is. Seen at an angle the
* near side of a brick then hides the mortar behind it. The same walk up towards the sun from the point found tells
* whether a bump shades it (self-shadowing).
*
* Everything is in the surface's own frame (tangent, bitangent, normal: the normal map's) and in texture units: the
* depth is a share of the texture's width (0.03 is a mortar groove 3% of the picture's width deep), so one number fits a
* texture at any size. Along the ray a fraction t of the way to the deepest point is at height 1 - t; the whole way down
* it has moved sideways by the view's sideways part over its part along the normal, times the depth.
*
* These functions mirror parallax.glsl; the searches' loops, which read the height map, are in the main pass
* (scene_surface.glsl), mirrored by parallaxSearch and parallaxShadow.
*/

constexpr int parallaxFewestSteps = PARALLAX_FEWEST_STEPS;
constexpr int parallaxMostSteps = PARALLAX_MOST_STEPS;
constexpr float parallaxSmallestViewCosine = static_cast<float>(PARALLAX_SMALLEST_VIEW_COSINE);
constexpr float parallaxFadeFullPixels = static_cast<float>(PARALLAX_FADE_FULL_PIXELS);
constexpr float parallaxFadeNonePixels = static_cast<float>(PARALLAX_FADE_NONE_PIXELS);
constexpr int parallaxShadowSteps = PARALLAX_SHADOW_STEPS;
constexpr float parallaxShadowSoftness = static_cast<float>(PARALLAX_SHADOW_SOFTNESS);

/*
* A direction in the surface's frame: x along the tangent, y along the bitangent (the tangent's sign w times the
* normal crossed with it, as normal mapping builds it), z along the normal. The normal is normalised and the tangent
* made square to it first, since interpolated ones are neither unit length nor quite square.
*/
Vec3 toTangentFrame(const Vec3& direction, const Vec3& normal, const Vec4& tangent);

// Steps for a view at this cosine to the normal: the fewest straight down, mostSteps along the surface, rounded.
int parallaxSteps(float viewCosine, int mostSteps);

// The shift in texture units over the whole depth for a view (towards the eye, in the surface's frame): away from the eye.
Vec2 parallaxDelta(const Vec3& viewTangent, float depth);

// The shift per unit of height for a ray rising towards a light (in the surface's frame): towards the light.
Vec2 parallaxLightDelta(const Vec3& lightTangent, float depth);

// How far the ray at t is above the ground of this height: positive above, zero or less once it has dropped below.
float parallaxGap(float t, float height);

/*
* Where between the last step above the ground (its gap gapAbove, positive) and the first below (gapBelow, zero or
* less) the ray crosses it, as a share of the step, with the ground between them taken as a straight line.
*/
float parallaxCrossing(float gapAbove, float gapBelow);

// The depth seen head-on in pixels: the depth over the larger of the texture coordinates' steps to the next pixel.
float parallaxDepthPixels(float depth, const Vec2& uvStepAcross, const Vec2& uvStepDown);

// How much of the depth to use at that many pixels: all from PARALLAX_FADE_FULL_PIXELS, none under PARALLAX_FADE_NONE_PIXELS.
float parallaxFade(float depthPixels);

/*
* One self-shadowing sample: the light's share past a bump of this height where the ray towards the light, having
* climbed this much since the point, is at rayHeight. All of it while the bump is below the ray; less by
* PARALLAX_SHADOW_SOFTNESS times how far it pokes above over the climb, none at all past that.
*/
float parallaxShadowSample(float terrainHeight, float rayHeight, float climbed);

// The height map, 0..1, at texture coordinates: what the main pass reads from the texture.
using ParallaxHeight = std::function<float(const Vec2&)>;

// What a search found: where its ray met the ground, and the ground's height there.
struct ParallaxHit {
    Vec2 uv;
    float height = 1.0f;
};

/*
* The search as the main pass runs it: from the top at uv, steps evenly down to the deepest point (each at t = i / steps,
* so the last is exactly the bottom, which every ray reaches), stopping at the first whose gap is zero or less, then the
* crossing between it and the step before. Where the ground is at the top at uv nothing moves.
*/
ParallaxHit parallaxSearch(const ParallaxHeight& heightAt, const Vec2& uv, const Vec3& viewTangent, float depth, int steps);

/*
* Self-shadowing as the main pass runs it: from the hit (at hitHeight) up towards the light to the top in steps, the
* light's share the least of the samples'. All of it for a light below the surface's plane (it is not lit at all
* then) or from the top.
*/
float parallaxShadow(const ParallaxHeight& heightAt, const Vec2& hitUv, float hitHeight, const Vec3& lightTangent, float depth, int steps);
