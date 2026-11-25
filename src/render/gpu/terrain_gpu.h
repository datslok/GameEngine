#pragma once

#include "scene/terrain_heights.h"

#include <cstdint>
#include <vector>

/*
* The terrain's records as the vertex shaders read them (terrain.glsl, scene/terrain.h): a patch each draw names
* (scene set 1, TerrainPatchesBinding, written each frame) and the heights (TerrainHeightsBinding, uploaded when they
* change). std430, sizes checked.
*/

// One patch: placement (corner x, corner z, size, quads), morph (start, end, spacing, unused), uv (corner x, corner z,
// per metre, unused).
struct GpuTerrainPatch {
    float placement[4];
    float morph[4];
    float uv[4];
};

static_assert(sizeof(GpuTerrainPatch) == 48, "GpuTerrainPatch must match terrain.glsl's TerrainPatch");

// One grid: its corner, spacing, points a side, where its heights start among all the grids' heights, one over its
// spacing as the CPU makes it (scene/terrain_heights.cpp multiplies by it), so both read the same squares, and where its
// pyramid's (lowest, highest) pairs start, after every grid's heights (TerrainHeightGrid::pyramid).
struct GpuTerrainGrid {
    float corner[2];
    float spacing;
    std::uint32_t size;
    std::uint32_t offset;
    float inverseSpacing;
    std::uint32_t pyramidOffset;
    std::uint32_t unused;
};

static_assert(sizeof(GpuTerrainGrid) == 32, "GpuTerrainGrid must match terrain.glsl's TerrainGrid");

// The heights buffer's start: how many grids, then each (finest first); every grid's heights follow it.
struct GpuTerrainHeightsHeader {
    std::uint32_t gridCount;
    std::uint32_t unused[3];
    GpuTerrainGrid grids[TERRAIN_MOST_GRIDS];
};

static_assert(sizeof(GpuTerrainHeightsHeader) == 16 + 32 * TERRAIN_MOST_GRIDS, "GpuTerrainHeightsHeader must match terrain.glsl's TerrainHeights");

GpuTerrainPatch packTerrainPatch(const TerrainPatch& patch);

// The heights buffer's words: its header, then each grid's heights' bits in order, then each grid's pyramid. Throws
// std::invalid_argument for more than TERRAIN_MOST_GRIDS grids.
std::vector<std::uint32_t> packTerrainHeights(const TerrainHeights& heights);
