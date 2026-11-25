// The terrain's pure maths (scene/terrain_heights.h mirrors it and is tested against it): CDLOD's morph (Strugar 2010).
// No buffer or texture reads here; terrain_data.glsl reads the heights and builds the vertex from these.
#ifndef TERRAIN_GLSL
#define TERRAIN_GLSL

#include "scene/terrain_shared.h"

// A patch as the shaders read it. Must match GpuTerrainPatch.
struct TerrainPatch {
    vec4 placement; // corner x, corner z, size, quads
    vec4 morph;     // start, end, spacing
    vec4 uv;        // corner x, corner z, per metre
};

// How far a point is through its patch's morph: 0 before start, 1 from end, linear between (terrainMorph).
float terrainMorph(float distance, float start, float end) {
    return clamp((distance - start) / (end - start), 0.0, 1.0);
}

// A grid point slid by the morph: an odd coordinate towards its even neighbour below, reaching it at 1 (morphGridCoordinate).
vec2 morphGridCoordinate(vec2 grid, float morph) {
    vec2 halved = grid * 0.5;
    return grid - (halved - floor(halved)) * 2.0 * morph;
}

// Bilinear between four heights: the near row (left, right), the far row, across and down in 0..1. Precise, so the
// planting reads the ground as the CPU does.
float terrainBilinear(float nearLeft, float nearRight, float farLeft, float farRight, float across, float down) {
    precise float upper = nearLeft + (nearRight - nearLeft) * across;
    precise float lower = farLeft + (farRight - farLeft) * across;
    precise float height = upper + (lower - upper) * down;
    return height;
}

// The height on the drawn grid square at a place (across and down in 0..1): the plane through the triangle holding it,
// the square split as the grid mesh splits it, from the near right corner to the far left (terrainTriangleHeight).
float terrainTriangleHeight(float nearLeft, float nearRight, float farLeft, float farRight, float across, float down) {
    precise float sum = across + down;

    if (sum <= 1.0) {
        precise float near = nearLeft + (nearRight - nearLeft) * across + (farLeft - nearLeft) * down;
        return near;
    }

    precise float far = farRight + (farLeft - farRight) * (1.0 - across) + (nearRight - farRight) * (1.0 - down);
    return far;
}

// Where a cluster of a patch's grid can stand, as (west, north, east, south): its footprint (low x, low z, high x, high
// z, 0 to 1 across) as grid points rounded out, one more towards the corner for the morph (terrainClusterBox).
vec4 terrainClusterBox(TerrainPatch landPatch, vec4 footprint) {
    float quads = landPatch.placement.w;
    vec2 low = max(floor(footprint.xy * quads) - 1.0, vec2(0.0));
    vec2 high = min(ceil(footprint.zw * quads), vec2(quads));
    return vec4(landPatch.placement.xy + low * landPatch.morph.z, landPatch.placement.xy + high * landPatch.morph.z);
}

// The sphere round a box on the land between two heights (x lowest, y highest), a hair larger for rounding
// (terrainBoxSphere).
vec4 terrainBoxSphere(vec4 box, vec2 heights) {
    vec3 low = vec3(box.x, heights.x, box.y);
    vec3 high = vec3(box.z, heights.y, box.w);
    vec3 extent = (high - low) * 0.5;
    vec3 reach = max(abs(low), abs(high));
    return vec4((low + high) * 0.5, length(extent) + 1e-5 * max(max(reach.x, reach.y), reach.z));
}

#endif
