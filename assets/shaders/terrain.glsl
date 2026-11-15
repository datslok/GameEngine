// The terrain's pure maths (scene/terrain_heights.h mirrors it and is tested against it): CDLOD's morph (Strugar 2010).
// No buffer or texture reads here; terrain_data.glsl reads the heights and builds the vertex from these.
#ifndef TERRAIN_GLSL
#define TERRAIN_GLSL

#include "scene/terrain_shared.h"

// How far a point is through its patch's morph: 0 before start, 1 from end, linear between (terrainMorph).
float terrainMorph(float distance, float start, float end) {
    return clamp((distance - start) / (end - start), 0.0, 1.0);
}

// A grid point slid by the morph: an odd coordinate towards its even neighbour below, reaching it at 1 (morphGridCoordinate).
vec2 morphGridCoordinate(vec2 grid, float morph) {
    vec2 halved = grid * 0.5;
    return grid - (halved - floor(halved)) * 2.0 * morph;
}

// Bilinear between four heights: the near row (left, right), the far row, across and down in 0..1.
float terrainBilinear(float nearLeft, float nearRight, float farLeft, float farRight, float across, float down) {
    float upper = nearLeft + (nearRight - nearLeft) * across;
    float lower = farLeft + (farRight - farLeft) * across;
    return upper + (lower - upper) * down;
}

#endif
