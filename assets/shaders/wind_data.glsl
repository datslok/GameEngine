// The wind force field as the scene's shaders read it (render/gpu/wind.h): its record and its springs, this frame's and
// last frame's (for motion vectors). Resource declarations only, included by the passes that sway what they draw.
#ifndef WIND_DATA_GLSL
#define WIND_DATA_GLSL

#include "wind.glsl"

layout(std430, set = 1, binding = 37) readonly buffer WindFieldRecord {
    WindField windField;
};

// Each texel a cell's springs, layer cascade * WIND_CATEGORIES + category: xy displacement, zw velocity. Sampled with
// wrapping, so a place reads its cell at (place / cell - 0.5) / WIND_CELLS whichever window texel holds it.
layout(set = 1, binding = 38) uniform sampler2DArray windSprings;
layout(set = 1, binding = 39) uniform sampler2DArray windPreviousSprings;

#endif
