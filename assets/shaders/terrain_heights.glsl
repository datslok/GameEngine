// The terrain's heights as a shader reads them (render/gpu/terrain_gpu.h's GpuTerrainHeightsHeader and the heights),
// mirroring scene/terrain_heights.cpp: terrainHeight and terrainSurfaceHeight exactly (times each grid's reciprocal
// spacing made on the CPU, every result precise), so the planting cells stand plants on the ground the CPU does. The
// buffer is the scene set's (set 1, binding 35) unless the includer names another first (TERRAIN_HEIGHTS_SET,
// TERRAIN_HEIGHTS_BINDING), as the planting passes do.
#ifndef TERRAIN_HEIGHTS_GLSL
#define TERRAIN_HEIGHTS_GLSL

#include "terrain.glsl"

#ifndef TERRAIN_HEIGHTS_SET
#define TERRAIN_HEIGHTS_SET 1
#define TERRAIN_HEIGHTS_BINDING 35
#endif

// Must match GpuTerrainGrid.
struct TerrainGrid {
    vec2 corner;
    float spacing;
    uint size;
    uint offset;
    float inverseSpacing;
    uint unused1;
    uint unused2;
};

// Must match GpuTerrainHeightsHeader followed by the heights.
layout(std430, set = TERRAIN_HEIGHTS_SET, binding = TERRAIN_HEIGHTS_BINDING) readonly buffer TerrainHeights {
    uint terrainGridCount;
    uint terrainUnused0;
    uint terrainUnused1;
    uint terrainUnused2;
    TerrainGrid terrainGrids[TERRAIN_MOST_GRIDS];
    float terrainHeights[];
};

// Whether a grid holds a place, its edges included.
bool terrainGridHolds(TerrainGrid grid, vec2 place) {
    precise float extent = float(grid.size - 1u) * grid.spacing;
    precise vec2 far = grid.corner + extent;
    return all(greaterThanEqual(place, grid.corner)) && all(lessThanEqual(place, far));
}

// The grid square round a place, clamped onto the grid: its first point's index and how far across and down it lies.
uint terrainGridSquare(TerrainGrid grid, vec2 place, out vec2 fraction) {
    float last = float(grid.size - 1u);
    precise vec2 cell = clamp((place - grid.corner) * grid.inverseSpacing, vec2(0.0), vec2(last));
    uvec2 first = min(uvec2(cell), uvec2(grid.size - 2u));
    precise vec2 within = cell - vec2(first);
    fraction = within;
    return grid.offset + first.y * grid.size + first.x;
}

// Bilinear between the four points round a place.
float terrainSampleGrid(TerrainGrid grid, vec2 place) {
    vec2 fraction;
    uint index = terrainGridSquare(grid, place, fraction);
    return terrainBilinear(terrainHeights[index], terrainHeights[index + 1u], terrainHeights[index + grid.size], terrainHeights[index + grid.size + 1u],
                           fraction.x, fraction.y);
}

// The drawn land's height in a grid at a place: its square's triangle there.
float terrainSampleGridSurface(TerrainGrid grid, vec2 place) {
    vec2 fraction;
    uint index = terrainGridSquare(grid, place, fraction);
    return terrainTriangleHeight(terrainHeights[index], terrainHeights[index + 1u], terrainHeights[index + grid.size],
                                 terrainHeights[index + grid.size + 1u], fraction.x, fraction.y);
}

// The drawn land's height at a place (terrainSurfaceHeight): as the finest patches draw it, their grid on the finest
// grid's points, each square two flat triangles; the finest grid holding it, else the coarsest's nearest edge.
float terrainSurfaceHeight(vec2 place) {
    for (uint grid = 0u; grid < terrainGridCount; ++grid) {
        if (terrainGridHolds(terrainGrids[grid], place)) {
            return terrainSampleGridSurface(terrainGrids[grid], place);
        }
    }

    return terrainGridCount > 0u ? terrainSampleGridSurface(terrainGrids[terrainGridCount - 1u], place) : 0.0;
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

#endif
