#pragma once

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
