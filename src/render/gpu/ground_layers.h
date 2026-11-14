#pragma once

#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/ground_layers_shared.h"

#include <array>
#include <cstdint>

/*
* A blended ground: up to four layer materials (grass, moss, leaf litter, mud, rock) painted over one mesh by a weights
* texture, each layer's own textures repeating at its own scale (Material::groundLayers). Cross-fading the layers by the
* painted weights alone looks like a smear where two meet. Height blending (as in "Advanced Terrain Texture Splatting",
* Mishkinis 2013) uses each layer's height map instead: where they meet, the texel standing higher shows, so a border
* follows the textures' own shapes, moss in the cracks between stones, grass poking through leaf litter.
*
* Each painted layer's share is its weight times how far its height comes within blendDepth of the highest painted
* layer's, normalised. A layer not painted never shows; equal heights keep the painted weights; a shallow depth gives
* sharp borders, a deep one the plain cross-fade. Mirrored in ground_layers.glsl.
*/
using GroundWeights = std::array<float, 4>;

// The layers' shares at one point from their painted weights (0..1, need not add up to one) and their heights (0..1).
// Nothing painted gives the first layer alone.
GroundWeights groundBlendWeights(const GroundWeights& painted, const GroundWeights& heights, float blendDepth);

/*
* Reading the painted weights smoothly. Read straight, the GPU blends the four nearest texels along straight lines, whose
* slope jumps at every texel border; the height blend then turns that into a sharp edge, so every border between layers
* comes out as a staircase along the texture's grid, however fine its texels (the forest, Nov 14 2026). A cubic B-spline
* through the sixteen nearest texels has no such kinks (its slope is continuous too), so its edges are smooth curves.
*
* It takes four straight reads (Sigg and Hadwiger 2005, GPU Gems 2, chapter 20): along each axis the four B-spline
* weights of the place's fraction f between texel centres, w0 = (1 - f)^3 / 6, w1 = (3f^3 - 6f^2 + 4) / 6,
* w2 = (-3f^3 + 3f^2 + 3f + 1) / 6, w3 = f^3 / 6, become two reads, each between two texels at the place that splits
* them by their weights: the first, weighted w0 + w1, at texel - 1 + w1 / (w0 + w1); the second, weighted w2 + w3, at
* texel + 1 + w3 / (w2 + w3). In and out in texels (texture coordinate times the texture's size: centres at k + 1/2).
* Mirrored in ground_layers.glsl.
*/
struct GroundPaintTaps {
    Vec2 first;
    Vec2 second;
    Vec2 firstShare; // the first read's weight along each axis; the second's is one less it
};

GroundPaintTaps groundPaintTaps(Vec2 texelPlace);

/*
* Projected layers (triplanar mapping), for shapes made by code whose texture coordinates are stretched with them: the
* sandstone pillars and floating islands, one mesh drawn hundreds of times at different sizes. Each layer is read three
* times instead, projected straight along the object's own axes, in metres (so it keeps its size however the object is
* scaled, and stays stuck to it when it moves), and the three blended by how squarely the surface faces each axis.
*
* The frame: the object's model matrix is a rotation R times a scale S (Transform's); a vertex p of the mesh is at S p in
* the object's own axes, in metres, and a normal there is R^T times the world's. Mirrored from triangle.vert.
*/
inline constexpr std::uint32_t groundProjectedFlag = GROUND_PROJECTED;
inline constexpr std::uint32_t groundStrataFlag = GROUND_STRATA;
inline constexpr float groundProjectionSmallest = static_cast<float>(GROUND_PROJECTION_SMALLEST);

struct ProjectionFrame {
    std::array<Vec3, 3> axes; // R's columns: the object's x, y and z in the world, unit length
    Vec3 scale;               // S: how much the object stretches along each of them
};

ProjectionFrame projectionFrame(const Mat4& model);

/*
* The three projections' shares from the normal in the object's frame: each axis's |n|^4, so a surface tilted a little
* off an axis is still almost all that axis's; then shares under groundProjectionSmallest are dropped and the rest scaled
* back to one (subtracting rather than cutting, so a share fades to nothing without a jump). A zero normal reads from above.
*/
Vec3 projectionWeights(const Vec3& frameNormal);

// Where projection along an axis (0 x, 1 y, 2 z) reads its texture, in metres: along x the plane (z, y), along y (x, z),
// along z (x, y), so up the picture is up the world on every side.
Vec2 projectedPlace(const Vec3& framePosition, int axis);

/*
* The bumped normal in the object's frame from each projection's normal map direction (decodeNormalTexel's, in that
* projection's plane: x along its first coordinate, y along its second, z out of it), blended by the weights. Each is
* combined with the surface's own normal by the whiteout blend (Barre-Brisebois and Hill 2012, as Golus 2017 applies it
* to triplanar mapping): the tilts added to the normal's sideways parts and the outward parts multiplied, the normal's
* own sign kept, so a flat map gives back the surface's normal on every side. Zero for no usable direction.
*/
Vec3 projectedNormal(const Vec3& frameNormal, const std::array<Vec3, 3>& tilts, const Vec3& weights);

/*
* Strata: the layers painted by height, a strip of weights from bottom to top (one cake of rock bands for a whole
* valley, so the same band stands at the same height on every pillar, as real eroded sandstone's does). Where along the
* strip (0 bottom, 1 top) a world height is, given the bottom and one over the strip's span.
*/
float strataPlace(float height, float bottom, float inverseSpan);

/*
* The strata's weights with a layer taking over where the surface faces up (moss on ledges and tops, where water and
* soil gather): none of it while the smooth normal's upward part is under topStart, all of it from topFull, smoothly
* between (a step at topStart if they are not in order). No top layer (topLayer < 0): the strata as they are.
*/
GroundWeights strataPaint(const GroundWeights& strata, int topLayer, float upward, float topStart, float topFull);

/*
* Wavy beds: real beds are not ruled lines, they rise and fall a few metres over hundreds. The height the strata are read
* at is moved by a slow noise texture over the world's x and z (its texel 0..1, 0.5 no move): up to amplitude metres
* either way.
*/
float warpedStrataHeight(float height, float warpTexel, float amplitude);

/*
* Water streaks: dark stains running down a cliff, where rain carries dirt and algae grow. A noise texture read on the
* sides' projections stretched along the height (one repeat every inverseWidth^-1 metres across, every inverseLength^-1
* down), so any blotch becomes a long vertical streak. Where along the streaks a point of the object's frame is, for a
* side projection (0 x, 2 z): across as projectedPlace's first coordinate, down as the height.
*/
Vec2 streakPlace(const Vec3& framePosition, int axis, float inverseWidth, float inverseLength);

/*
* How much a streak darkens the albedo (a factor, 1 none): its texel (1 clean rock, 0 the darkest stain) times strength,
* only on the sides (sideShare: the side projections' shares, 0 on what faces straight up or down).
*/
float streakShade(float streakTexel, float strength, float sideShare);
