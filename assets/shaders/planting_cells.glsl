// The planting cells' passes' shared part (render/gpu/planting_records.h, render/vulkan/vulkan_planting.h): their
// buffers, the maps read as scene/planting_maps.cpp reads them, and a cell's candidate as its candidateAt makes it, line
// for line, every result precise, so the GPU plants the CPU's plants bit for bit (placement-compare checks it). A shader
// including it turns on GL_EXT_shader_explicit_arithmetic_types_int64.

#ifndef PLANTING_CELLS_GLSL
#define PLANTING_CELLS_GLSL

#include "placed_plant.glsl"
#include "planting.glsl"
#include "render/gpu/planting_records_shared.h"

#define TERRAIN_HEIGHTS_SET 0
#define TERRAIN_HEIGHTS_BINDING PLANTING_HEIGHTS_BINDING
#include "terrain_heights.glsl"

// Must match GpuPlantingMapsHeader followed by the maps' words.
layout(std430, set = 0, binding = PLANTING_MAPS_BINDING) readonly buffer PlantingMapsBuffer {
    int mapColumns;
    int mapRows;
    float texelMetres;
    float inverseTexel;
    float halfWidth;
    float northmost;
    float texelReach;
    float corridorClearance;
    uint axisFirst;
    uint axisCount;
    uint ecotopesFirst;
    uint gapsFirst;
    uint pathFirst;
    uint clearanceFirst;
    uint pathPointsFirst;
    uint pathPieces;
    uint pathInverseFirst;
    uint capsFirst;
    uint capCount;
    uint mapFlags;
    uint mapWords[];
};

// Must match GpuPlantingLayer.
struct PlantingLayer {
    vec3 sharePerWeight;
    float gapStep;
    float nearPath;
    float farPath;
    float farShare;
    float inversePathSpan;
    float footprint;
    float jitterMetres;
    float pathClearance;
    int under;
    uvec2 seed;
    int kinds;
    uint keepAwayCount;
    vec4 kindShares;
    float leastScale;
    float scaleRange;
    float footRingPerScale;
    float footSink;
    float spacingShare;
    int spacingRings;
    uint sizes;
    uint sharedKinds;
    ivec4 keepLayer;
    vec4 keepRadius;
    vec4 keepReach;
    uvec4 keepCrown;
    float reachPerScale[PLANTING_MOST_KINDS];
    float centrePerScale[PLANTING_MOST_KINDS];
};

layout(std430, set = 0, binding = PLANTING_LAYERS_BINDING) readonly buffer PlantingLayers {
    PlantingLayer plantingLayers[];
};

// Must match GpuPlantingJob: lattices as (first column, first row, columns, rows), bounds (west, east, north, south).
struct PlantingJob {
    ivec4 candidates;
    ivec4 standing;
    ivec4 box;
    vec4 bounds;
    uint candidateFirst;
    uint plantFirst;
    uint countPlace;
    uint capacity;
};

layout(std430, set = 0, binding = PLANTING_JOBS_BINDING) readonly buffer PlantingJobs {
    PlantingJob plantingJobs[];
};

// Must match GpuPlantingCandidate.
struct PlantingCandidate {
    vec4 footScale;
    vec4 turnPickReachThreshold;
    uint kind;
    uint candidate;
    uint kept;
    uint unused;
};

layout(std430, set = 0, binding = PLANTING_CANDIDATES_BINDING) buffer PlantingCandidates {
    PlantingCandidate plantingCandidates[];
};

// The slots' plants and counts: written by the planting's passes, read by the placed drawing's (which define
// PLANTING_SLOTS_READ before including this).
#ifdef PLANTING_SLOTS_READ
#define PLANTING_SLOTS_ACCESS readonly
#else
#define PLANTING_SLOTS_ACCESS writeonly
#endif

layout(std430, set = 0, binding = PLANTING_PLANTS_BINDING) PLANTING_SLOTS_ACCESS buffer PlacedPlants {
    PlacedPlant placedPlants[];
};

layout(std430, set = 0, binding = PLANTING_COUNTS_BINDING) PLANTING_SLOTS_ACCESS buffer PlantingCounts {
    uint plantingCounts[];
};

layout(std430, set = 0, binding = PLANTING_PROBLEMS_BINDING) buffer PlantingProblems {
    uint plantingProblems[PLANTING_PROBLEMS];
};

// The pass's layer, how many layers each cell has (its jobs' stride) and how many cells are made this frame.
layout(push_constant) uniform PlantingPass {
    uint passLayer;
    uint layerCount;
    uint cellCount;
    uint passUnused;
};

void plantingProblem(uint problem) {
    atomicAdd(plantingProblems[problem], 1u);
}

// ---- Lattices ----

bool latticeHolds(ivec4 lattice, int column, int row) {
    return column >= lattice.x && column < lattice.x + lattice.z && row >= lattice.y && row < lattice.y + lattice.w;
}

uint latticeIndex(ivec4 lattice, int column, int row) {
    return uint(row - lattice.y) * uint(lattice.z) + uint(column - lattice.x);
}

uint latticeCount(ivec4 lattice) {
    return uint(lattice.z) * uint(lattice.w);
}

PlantingJob jobOf(uint cell, uint layer) {
    return plantingJobs[cell * layerCount + layer];
}

// ---- The maps (plantingAxisAt to plantingFitsTheMaps) ----

float mapFloat(uint word) {
    return uintBitsToFloat(mapWords[word]);
}

uint mapByte(uint first, uint texel) {
    return (mapWords[first + texel / 4u] >> (8u * (texel % 4u))) & 0xffu;
}

uint mapHalf(uint first, uint texel) {
    return (mapWords[first + texel / 2u] >> (16u * (texel % 2u))) & 0xffffu;
}

float plantingAxisAt(float z) {
    if (axisCount == 0u) {
        return 0.0;
    }

    precise float row = clamp(plantingCoordinate(z, northmost, inverseTexel) - 0.5, 0.0, float(axisCount - 1u));
    uint north = uint(row);
    uint south = min(north + 1u, axisCount - 1u);
    precise float share = row - float(north);
    return plantingBetween(mapFloat(axisFirst + north), mapFloat(axisFirst + south), share);
}

// Whether a foot is in a region (north, south, spread): insidePlantedRegion.
bool plantingInsideRegion(vec3 region, vec3 foot) {
    precise float across = abs(foot.x - plantingAxisAt(foot.z));
    return foot.z >= region.x && foot.z <= region.y && across <= region.z;
}

vec2 plantingGridPlace(float x, float z) {
    precise float across = x - plantingAxisAt(z);
    return vec2(plantingCoordinate(across, -halfWidth, inverseTexel), plantingCoordinate(z, northmost, inverseTexel));
}

bool outsideTheMaps(float x, float z) {
    precise float south = northmost + float(mapRows) * texelMetres;
    precise float across = abs(x - plantingAxisAt(z));
    return mapRows <= 0 || mapColumns <= 0 || across > halfWidth || z < northmost || z > south;
}

// The four texels round a place (north-west, north-east, south-west, south-east) and how far across and down.
struct MapBlend {
    uvec4 texels;
    float across;
    float down;
};

MapBlend blendAt(float x, float z) {
    vec2 grid = plantingGridPlace(x, z);
    precise float column = clamp(grid.x - 0.5, 0.0, float(mapColumns - 1));
    precise float row = clamp(grid.y - 0.5, 0.0, float(mapRows - 1));
    int west = int(column);
    int north = int(row);
    int east = min(west + 1, mapColumns - 1);
    int south = min(north + 1, mapRows - 1);
    uint columns = uint(mapColumns);
    MapBlend blend;
    blend.texels = uvec4(uint(north) * columns + uint(west), uint(north) * columns + uint(east), uint(south) * columns + uint(west),
                         uint(south) * columns + uint(east));
    precise float across = column - float(west);
    precise float down = row - float(north);
    blend.across = across;
    blend.down = down;
    return blend;
}

float blended(MapBlend blend, vec4 values) {
    return terrainBilinear(values.x, values.y, values.z, values.w, blend.across, blend.down);
}

vec4 plantingEcotopes(float x, float z) {
    if ((mapFlags & PLANTING_HAS_ECOTOPES) == 0u || outsideTheMaps(x, z)) {
        return vec4(0.0, 0.0, 0.0, 255.0);
    }

    MapBlend blend = blendAt(x, z);
    vec4 weights;

    for (uint which = 0u; which < 4u; ++which) {
        vec4 values;

        for (uint corner = 0u; corner < 4u; ++corner) {
            values[corner] = float((mapWords[ecotopesFirst + blend.texels[corner]] >> (8u * which)) & 0xffu);
        }

        weights[which] = blended(blend, values);
    }

    return weights;
}

float plantingGap(float x, float z) {
    if ((mapFlags & PLANTING_HAS_GAPS) == 0u || outsideTheMaps(x, z)) {
        return 0.0;
    }

    MapBlend blend = blendAt(x, z);
    return blended(blend, vec4(float(mapByte(gapsFirst, blend.texels.x)), float(mapByte(gapsFirst, blend.texels.y)),
                               float(mapByte(gapsFirst, blend.texels.z)), float(mapByte(gapsFirst, blend.texels.w))));
}

float plantingPathDistanceAt(float x, float z) {
    if ((mapFlags & PLANTING_HAS_PATH) == 0u || outsideTheMaps(x, z)) {
        return plantingPathDistance(float(PLANTING_FARTHEST_PATH));
    }

    MapBlend blend = blendAt(x, z);
    return plantingPathDistance(blended(blend, vec4(float(mapHalf(pathFirst, blend.texels.x)), float(mapHalf(pathFirst, blend.texels.y)),
                                                    float(mapHalf(pathFirst, blend.texels.z)), float(mapHalf(pathFirst, blend.texels.w)))));
}

vec2 pathPoint(uint point) {
    return vec2(mapFloat(pathPointsFirst + 2u * point), mapFloat(pathPointsFirst + 2u * point + 1u));
}

bool plantingClearOfThePath(vec2 place, float clearance) {
    for (uint piece = 0u; piece < pathPieces; ++piece) {
        vec2 start = pathPoint(piece);
        vec2 next = pathPoint(piece + 1u);
        precise vec2 along = vec2(next.x - start.x, next.y - start.y);

        if (plantingNearSegment(place, start, along, mapFloat(pathInverseFirst + piece), clearance)) {
            return false;
        }
    }

    return true;
}

float plantingClearance(float x, float z) {
    vec2 grid = plantingGridPlace(x, z);
    int column = int(floor(grid.x));
    int row = int(floor(grid.y));

    if ((mapFlags & PLANTING_HAS_CLEARANCE) == 0u || column < 0 || column >= mapColumns || row < 0 || row >= mapRows) {
        return float(PLANTING_FARTHEST_CLEARANCE);
    }

    uint stored = mapHalf(clearanceFirst, uint(row) * uint(mapColumns) + uint(column));
    return plantingStored(float(stored), float(PLANTING_DECIMETRE), texelReach);
}

// A wider plant than every cap is counted as a problem (the CPU throws) and read as uncapped.
float plantingCap(float reach, float x, float z) {
    for (uint cap = 0u; cap < capCount; ++cap) {
        uint at = capsFirst + cap * PLANTING_CAP_WORDS;

        if (mapFloat(at) < reach) {
            continue;
        }

        int columns = int(mapWords[at + 3u]);
        int rows = int(mapWords[at + 4u]);
        int column = int(floor(plantingCoordinate(x, mapFloat(at + 1u), inverseTexel)));
        int row = int(floor(plantingCoordinate(z, mapFloat(at + 2u), inverseTexel)));

        if (column < 0 || column >= columns || row < 0 || row >= rows) {
            return float(PLANTING_UNCAPPED);
        }

        uint stored = mapHalf(mapWords[at + 5u], uint(row) * uint(columns) + uint(column));
        return stored == PLANTING_NO_CAP ? float(PLANTING_UNCAPPED)
                                         : plantingStored(float(stored), float(PLANTING_DECIMETRE), float(PLANTING_CAP_FLOOR_METRES));
    }

    plantingProblem(PLANTING_PROBLEM_NO_CAP);
    return float(PLANTING_UNCAPPED);
}

bool plantingFitsTheMaps(vec2 place, float reach, float top) {
    return plantingFits(plantingClearance(place.x, place.y), reach, top, corridorClearance, plantingCap(reach, place.x, place.y));
}

float plantingLayerShare(uint layer, float x, float z) {
    PlantingLayer record = plantingLayers[layer];
    vec4 weights = plantingEcotopes(x, z);
    float pathShare = plantingPathShare(plantingPathDistanceAt(x, z), record.nearPath, record.farPath, record.farShare, record.inversePathSpan);
    return plantingShare(weights.xyz, record.sharePerWeight, plantingGap(x, z), record.gapStep, pathShare);
}

// ---- A cell's candidate (candidateAt) ----

uint64_t layerSeed(PlantingLayer layer) {
    return uint64_t(layer.seed.x) | (uint64_t(layer.seed.y) << 32);
}

// Whether a plant an earlier layer kept stands within one of this layer's keep-aways of a place (keptAway): reading
// past what that layer kept is counted as a problem (the CPU throws).
bool keptAway(PlantingLayer layer, uint keep, uint cell, vec2 place) {
    uint other = uint(layer.keepLayer[keep]);
    float footprint = plantingLayers[other].footprint;
    float reach = layer.keepReach[keep];
    PlantingJob job = jobOf(cell, other);
    int firstColumn = int(floor((place.x - reach) / footprint)) - 1;
    int lastColumn = int(floor((place.x + reach) / footprint)) + 1;
    int firstRow = int(floor((place.y - reach) / footprint)) - 1;
    int lastRow = int(floor((place.y + reach) / footprint)) + 1;

    if (!latticeHolds(job.standing, firstColumn, firstRow) || !latticeHolds(job.standing, lastColumn, lastRow)) {
        plantingProblem(PLANTING_PROBLEM_READ_PAST);
        return false;
    }

    bool crown = layer.keepCrown[keep] != 0u;

    for (int row = firstRow; row <= lastRow; ++row) {
        for (int column = firstColumn; column <= lastColumn; ++column) {
            PlantingCandidate plant = plantingCandidates[job.candidateFirst + latticeIndex(job.candidates, column, row)];

            if (plant.kept != 0u && plantingWithin(place, plant.footScale.xz, crown ? plant.turnPickReachThreshold.z : layer.keepRadius[keep])) {
                return true;
            }
        }
    }

    return false;
}

// A cell's plant before its own layer's spacing, as candidateAt makes it: none where the dither, the path, a layer
// before or the maps refuse (candidate 0).
PlantingCandidate plantingCandidateAt(uint layerIndex, uint cell, int column, int row) {
    PlantingLayer layer = plantingLayers[layerIndex];
    PlantingCandidate none;
    none.footScale = vec4(0.0);
    none.turnPickReachThreshold = vec4(0.0);
    none.kind = 0u;
    none.candidate = 0u;
    none.kept = 0u;
    none.unused = 0u;

    float footprint = layer.footprint;
    float threshold = plantingThreshold(column, row);
    precise float middleX = (float(column) + 0.5) * footprint;
    precise float middleZ = (float(row) + 0.5) * footprint;
    float below = layer.under >= 0 ? plantingLayerShare(uint(layer.under), middleX, middleZ) : 0.0;

    if (!plantingTaken(threshold, below, plantingLayerShare(layerIndex, middleX, middleZ))) {
        return none;
    }

    uint64_t seed = layerSeed(layer);
    uint64_t firstDraw = plantingCellKey(column, row) * uint64_t(PLANTING_DRAWS_PER_CELL);
    vec2 place = plantingJitteredPlace(column, row, footprint, layer.jitterMetres, plantingRandom(seed, firstDraw), plantingRandom(seed, firstDraw + 1UL));

    if (!plantingClearOfThePath(place, layer.pathClearance)) {
        return none;
    }

    for (uint keep = 0u; keep < layer.keepAwayCount; ++keep) {
        if (keptAway(layer, keep, cell, place)) {
            return none;
        }
    }

    uint64_t draws = firstDraw + uint64_t(PLANTING_DRAWS_USED);
    float scale = plantingScale(layer.leastScale, layer.scaleRange, plantingRandom(seed, draws + uint64_t(PLANTING_DRAW_SCALE)));
    float turn = plantingTurn(plantingRandom(seed, draws + uint64_t(PLANTING_DRAW_TURN)));
    float kindDraw = plantingRandom(seed, draws + uint64_t(PLANTING_DRAW_KIND));
    int kind = layer.sharedKinds == 0u ? plantingUniformKind(kindDraw, layer.kinds) : plantingSharedKind(kindDraw, layer.kindShares, layer.kinds);
    float pick = plantingRandom(seed, draws + uint64_t(PLANTING_DRAW_PICK));
    uint size = layer.sizes == 1u ? 0u : uint(kind);
    precise float reach = layer.reachPerScale[size] * scale;

    float ground = terrainSurfaceHeight(place);

    if (layer.footRingPerScale > 0.0) {
        precise float ring = layer.footRingPerScale * scale;

        for (int k = 0; k < PLANTING_RING_POINTS; ++k) {
            ground = min(ground, terrainSurfaceHeight(plantingRingPoint(place, ring, k)));
        }
    }

    precise float footHeight = ground - layer.footSink;

    if (!plantingFitsTheMaps(place, reach, plantingTop(footHeight, layer.centrePerScale[size], scale, reach))) {
        return none;
    }

    PlantingCandidate plant;
    plant.footScale = vec4(place.x, footHeight, place.y, scale);
    plant.turnPickReachThreshold = vec4(turn, pick, reach, threshold);
    plant.kind = uint(kind);
    plant.candidate = 1u;
    plant.kept = 0u;
    plant.unused = 0u;
    return plant;
}

#endif
