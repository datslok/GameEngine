#pragma once

#include "math/vec2.h"

#include <array>

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
