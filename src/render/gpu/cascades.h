#pragma once

#include "math/mat4.h"
#include "math/vec3.h"
#include "render/gpu/shader_shared.h"
#include "scene/camera.h"
#include "scene/frame_description.h"

#include <cstdint>
#include <optional>
#include <vector>

/*
* Cascaded shadow maps for the sun. One shadow map cannot be sharp both a metre and a kilometre away: its texels are the
* same size everywhere, while what the camera sees shrinks with distance. So the view is cut into slices along its depth
* (cascades), and each gets its own box-shaped shadow view, just big enough for its slice: the near slice is small and
* sharp, the far ones large and coarse, which roughly matches how much of the screen a metre covers at each distance.
*
* Each slice is wrapped in its smallest bounding sphere, and the box around that sphere moves in whole texels only. The
* sphere's size depends only on the lens (field of view, aspect ratio, the slice's distances), not on where the camera
* points, so the box keeps one size as the camera turns, and with whole-texel steps every shadow edge stays on the same
* texels instead of crawling and shimmering.
*
* A point is shadowed by the finest cascade whose box holds it (chosen by box, not by distance: the boxes overlap, and
* the finer one often reaches past its own slice). Near a box's edge it is blended with the next cascade over a band, so
* there is no visible seam; the last cascade fades to lit. The shader's side of this is assets/shaders/cascades.glsl,
* mirrored here and tested.
*/

// The most cascades a sun can have: four of the largest tiles, a quarter of the shadow atlas (shadow_map.h).
inline constexpr int maxShadowCascades = 4;

/*
* The distances along the view (from the camera, along its forward axis) where cascades meet: count + 1 of them, from
* nearDistance to farDistance. lambda blends evenly spaced splits (0) with logarithmic ones (1), which keep each texel's
* size on screen about the same but leave the near cascade tiny; between them is the usual compromise ("practical"
* splits, Zhang et al. 2006).
*/
std::vector<float> cascadeSplits(float nearDistance, float farDistance, int count, float lambda);

/*
* The smallest sphere holding the part of the camera's view between two distances along it. Its centre lies on the
* view's axis, where the slice's near and far corners are equally far away, or at the far end's centre when that is
* nearer (a wide, shallow slice).
*/
BoundingSphere cascadeSphere(const Camera& camera, float nearDistance, float farDistance);

/*
* Sample distribution shadow maps (Lauritzen 2011): the cascades end at the farthest surface on screen instead of at the
* shadow distance, so in a room they share out their texels over metres, not the 200 m the view could reach. The
* farthest surface comes from the depth image a frame or two late (render/gpu/depth_range.h), and every cascade's box
* is sized by where its slice ends, so if the fitted distance followed every small change, every shadow texel would change
* size every frame: shadow edges would shimmer, and no cascade could stay cached. So the distance moves on a ladder of
* fixed steps, each shadowDistanceStep times the last from 1 m: it climbs at once when more comes into view (rounded up,
* after a margin for the frames it is late), but comes down only once the need is shadowDistanceShrinkSteps lower, so a
* view hovering at a step's edge does not flip between two sizes. Only the far end is fitted: starting the splits at the
* nearest surface instead of the camera's near plane pushes them all outwards, which makes the nearest surfaces coarser.
*/
inline constexpr float shadowDistanceStep = 1.25f;
inline constexpr float shadowDistanceMargin = 1.1f;
inline constexpr int shadowDistanceShrinkSteps = 2;

// Which step of the ladder the cascades end at (shadowDistanceStep to its power, in metres); none when not fitted.
struct ShadowDistanceFit {
    std::optional<int> step;
};

// The fit after a new measurement of the farthest surface's distance along the view: none (no surface on screen)
// leaves the cascades unfitted, as before anything was measured.
ShadowDistanceFit fitShadowDistance(const ShadowDistanceFit& previous, std::optional<float> farthestSurface);

// Where the fitted cascades end: infinity when not fitted, so the shadow distance applies.
float fittedShadowDistance(const ShadowDistanceFit& fit);

// Any up direction works for a shadow view, as long as it is not parallel to the way the view looks.
Vec3 shadowViewUp(const Vec3& forward);

// One cascade's shadow view: its view-projection (the GPU's depth range), how deep the box is from its near side
// (towards the light) to its far side, and the world size of one texel.
struct CascadeBox {
    Mat4 matrix = Mat4::identity();
    float depthRange = 0.0f;
    float texelSize = 0.0f;
};

/*
* A box looking along the light around the sphere, reaching casterReach further back towards the light so that tall
* things outside the view still cast into it. Its centre moves across the light in whole texels of a tile of tileSize.
*/
CascadeBox cascadeBox(const Vec3& lightDirection, const BoundingSphere& sphere, float casterReach, std::uint32_t tileSize);

/*
* How far a point is inside a cascade's box, in units of the cascade's radius: 1 at the centre, 0 on the box's edge,
* negative outside. projected is the point in the box's view (x and y from -1 to 1, depth from 1 at the side towards the
* light to 0 at the far side), depthRangeOverRadius the box's depth over its radius, to measure depth in the same units.
*/
float cascadeEdgeDistance(const Vec3& projected, float depthRangeOverRadius);

// How much of a point's shadow comes from this cascade rather than the next: 1 deeper than blendBand inside its box,
// falling to 0 at the edge. A band of 0 or less switches straight from one to the next.
float cascadeBlendWeight(float edgeDistance, float blendBand);

// For the dithered switch: whether this pixel, with its noise in 0..1, takes its shadow from this cascade alone.
// A share of pixels equal to the weight do, so on average the switch blends like the band.
bool cascadeDitherKeeps(float weight, float noise);

// The steepest slope the sun's bias grows with (see sunShadowBias). Shared with the shader.
inline constexpr float maxSunBiasSlope = static_cast<float>(MAX_SUN_BIAS_SLOPE);

/*
* How far to move a point towards the sun before looking up its shadow, so a surface does not shadow itself (acne).
* Moving along the sun's rays changes only the point's depth in a cascade's map, never where it lands, so the bias
* cannot shift a shadow's edge, and cascades with different texel sizes agree on where every edge is (an offset along
* the normal moves the point sideways too, by an amount that grows with the texel, which put the edge in a different
* place in each cascade). baseOffset is the cascade's offset for a surface facing the sun; a surface at an angle needs
* more, since one texel of the map then spans more depth on it: the tangent of the angle more, up to maxSunBiasSlope.
* nDotL is the cosine of that angle.
*/
float sunShadowBias(float baseOffset, float nDotL);
