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
    uint pyramidOffset; // its min/max pyramid's (lowest, highest) pairs, after every grid's heights
    uint unused;
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

// A grid's pyramid levels above its points, a level's texels a side and where its pairs start (terrainPyramidLevels,
// terrainPyramidLevelSize, terrainPyramidLevelStart).
uint terrainPyramidLevelSize(uint size, uint level) {
    return (size + (1u << level) - 1u) >> level;
}

uint terrainPyramidLevelStart(uint size, uint level) {
    uint start = 0u;

    for (uint below = 1u; below < level; ++below) {
        uint across = terrainPyramidLevelSize(size, below);
        start += across * across;
    }

    return start;
}

// The lowest and highest under a texel of a level, level 0 the point itself (terrainPyramidTexel).
vec2 terrainPyramidTexel(TerrainGrid grid, uint level, uint column, uint row) {
    if (level == 0u) {
        float height = terrainHeights[grid.offset + row * grid.size + column];
        return vec2(height);
    }

    uint pair = terrainPyramidLevelStart(grid.size, level) + row * terrainPyramidLevelSize(grid.size, level) + column;
    return vec2(terrainHeights[grid.pyramidOffset + 2u * pair], terrainHeights[grid.pyramidOffset + 2u * pair + 1u]);
}

// A place's point along one axis of a grid, clamped onto it, rounded down or up.
uint terrainGridPoint(float place, float corner, TerrainGrid grid, bool upper) {
    float coordinate = clamp((place - corner) / grid.spacing, 0.0, float(grid.size - 1u));
    return uint(upper ? ceil(coordinate) : floor(coordinate));
}

/*
* Every height terrainHeight or terrainSurfaceHeight can give in the box from low (west, north) to high (east, south),
* as (lowest, highest): each grid's points round its squares, one wider each way, read as four texels of its pyramid
* where they span at most two each way, from the finest grid up to the first holding the whole box, a hair wider for
* rounding; (0, 0) without grids (terrainHeightRange).
*/
vec2 terrainHeightRange(vec2 low, vec2 high) {
    vec2 range = vec2(3.4e38, -3.4e38);

    for (uint index = 0u; index < terrainGridCount; ++index) {
        TerrainGrid grid = terrainGrids[index];
        uint firstColumn = max(terrainGridPoint(low.x, grid.corner.x, grid, false), 1u) - 1u;
        uint lastColumn = min(terrainGridPoint(high.x, grid.corner.x, grid, true) + 1u, grid.size - 1u);
        uint firstRow = max(terrainGridPoint(low.y, grid.corner.y, grid, false), 1u) - 1u;
        uint lastRow = min(terrainGridPoint(high.y, grid.corner.y, grid, true) + 1u, grid.size - 1u);
        uint level = 0u;

        while ((firstColumn >> level) + 1u < (lastColumn >> level) || (firstRow >> level) + 1u < (lastRow >> level)) {
            ++level;
        }

        for (uint corner = 0u; corner < 4u; ++corner) {
            uint column = ((corner & 1u) != 0u ? lastColumn : firstColumn) >> level;
            uint row = ((corner & 2u) != 0u ? lastRow : firstRow) >> level;
            vec2 texel = terrainPyramidTexel(grid, level, column, row);
            range = vec2(min(range.x, texel.x), max(range.y, texel.y));
        }

        if (terrainGridHolds(grid, low) && terrainGridHolds(grid, high)) {
            break;
        }
    }

    if (range.x > range.y) {
        return vec2(0.0);
    }

    float slack = 1e-5 * max(1.0, max(abs(range.x), abs(range.y)));
    return vec2(range.x - slack, range.y + slack);
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
