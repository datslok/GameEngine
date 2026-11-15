// The terrain's heights and patches as the scene's vertex shaders read them (render/gpu/terrain_gpu.h), and the patch
// vertex built from them: terrainHeight, terrainNormal and terrainVertex mirror scene/terrain_heights.cpp. Included by
// triangle.vert and shadow.vert.
#ifndef TERRAIN_DATA_GLSL
#define TERRAIN_DATA_GLSL

#include "terrain.glsl"

// Must match GpuTerrainGrid.
struct TerrainGrid {
    vec2 corner;
    float spacing;
    uint size;
    uint offset;
    uint unused0;
    uint unused1;
    uint unused2;
};

// Must match GpuTerrainHeightsHeader followed by the heights.
layout(std430, set = 1, binding = 35) readonly buffer TerrainHeights {
    uint terrainGridCount;
    uint terrainUnused0;
    uint terrainUnused1;
    uint terrainUnused2;
    TerrainGrid terrainGrids[TERRAIN_MOST_GRIDS];
    float terrainHeights[];
};

// Must match GpuTerrainPatch.
struct TerrainPatch {
    vec4 placement; // corner x, corner z, size, quads
    vec4 morph;     // start, end, spacing
    vec4 uv;        // corner x, corner z, per metre
};

layout(std430, set = 1, binding = 36) readonly buffer TerrainPatches {
    TerrainPatch terrainPatches[];
};

// Whether a grid holds a place, its edges included.
bool terrainGridHolds(TerrainGrid grid, vec2 place) {
    float extent = float(grid.size - 1u) * grid.spacing;
    return all(greaterThanEqual(place, grid.corner)) && all(lessThanEqual(place, grid.corner + extent));
}

// Bilinear between the four points round a place, clamped onto the grid.
float terrainSampleGrid(TerrainGrid grid, vec2 place) {
    float last = float(grid.size - 1u);
    vec2 cell = clamp((place - grid.corner) / grid.spacing, vec2(0.0), vec2(last));
    uvec2 first = min(uvec2(cell), uvec2(grid.size - 2u));
    vec2 fraction = cell - vec2(first);
    uint index = grid.offset + first.y * grid.size + first.x;
    return terrainBilinear(terrainHeights[index], terrainHeights[index + 1u], terrainHeights[index + grid.size], terrainHeights[index + grid.size + 1u],
                           fraction.x, fraction.y);
}

// The land's height at a place: the finest grid holding it, else the coarsest's nearest edge.
float terrainHeight(vec2 place) {
    for (uint grid = 0u; grid < terrainGridCount; ++grid) {
        if (terrainGridHolds(terrainGrids[grid], place)) {
            return terrainSampleGrid(terrainGrids[grid], place);
        }
    }

    return terrainGridCount > 0u ? terrainSampleGrid(terrainGrids[terrainGridCount - 1u], place) : 0.0;
}

// The land's unit normal from its slope over step metres either way.
vec3 terrainNormal(vec2 place, float step) {
    float slopeX = (terrainHeight(place + vec2(step, 0.0)) - terrainHeight(place - vec2(step, 0.0))) / (2.0 * step);
    float slopeZ = (terrainHeight(place + vec2(0.0, step)) - terrainHeight(place - vec2(0.0, step))) / (2.0 * step);
    return normalize(vec3(-slopeX, 1.0, -slopeZ));
}

// A patch's vertex: where it is, its normal, its tangent (w the bitangent's side) and its texture coordinates.
struct TerrainVertex {
    vec3 position;
    vec3 normal;
    vec4 tangent;
    vec2 uv;
};

// The vertex at a grid mesh's point (x and z, 0 to 1 across), seen from eye (terrainVertex).
TerrainVertex terrainVertex(TerrainPatch landPatch, vec2 meshPoint, vec3 eye) {
    vec2 corner = landPatch.placement.xy;
    float spacing = landPatch.morph.z;
    vec2 grid = round(meshPoint * landPatch.placement.w);
    vec2 start = corner + grid * spacing;
    float morph = terrainMorph(length(vec3(start.x, terrainHeight(start), start.y) - eye), landPatch.morph.x, landPatch.morph.y);
    vec2 place = corner + morphGridCoordinate(grid, morph) * spacing;
    float step = spacing * (1.0 + morph);

    TerrainVertex vertex;
    vertex.position = vec3(place.x, terrainHeight(place), place.y);
    vertex.normal = terrainNormal(place, step);
    vertex.tangent = vec4(normalize(vec3(2.0 * step, terrainHeight(place + vec2(step, 0.0)) - terrainHeight(place - vec2(step, 0.0)), 0.0)), -1.0);
    vertex.uv = (place - landPatch.uv.xy) * landPatch.uv.z;
    return vertex;
}

#endif
