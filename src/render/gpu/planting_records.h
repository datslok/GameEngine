#pragma once

#include "render/gpu/planting_records_shared.h"
#include "scene/planting.h"
#include "scene/planting_cells.h"
#include "scene/planting_maps.h"

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

/*
* The planting cells' records on the GPU (ROADMAP phase 18, the valley's stage 2b; docs/architecture/planting.md):
* the maps and layers as planting_cells.glsl reads them, a job a layer of each cell made this frame, the candidates and
* the placed plants the passes write, and the comparison of the GPU's cells with plantLayers' (placement-compare).
* std430, sizes checked.
*/

inline constexpr int plantingMostKeepAways = PLANTING_MOST_KEEP_AWAYS;

/*
* The maps' buffer starts with this; their words follow, each map's first word given (counted from the words' start):
* the axis (a float a row), the ecotopes (a word a texel, floor to bare from the low byte), gaps (four texels a word),
* the path's distances and the clearance (two texels a word, the first low), the path's points (x, z) and each piece's
* inverse length squared, the caps (PLANTING_CAP_WORDS each) and their heights (two a word), the cover maps
* (PLANTING_COVER_MAP_WORDS each) and their bytes (four a word).
*/
struct GpuPlantingMapsHeader {
    std::int32_t columns;
    std::int32_t rows;
    float texelMetres;
    float inverseTexel;
    float halfWidth;
    float northmost;
    float texelReach;
    float corridorClearance;
    std::uint32_t axisFirst;
    std::uint32_t axisCount;
    std::uint32_t ecotopesFirst;
    std::uint32_t gapsFirst;
    std::uint32_t pathFirst;
    std::uint32_t clearanceFirst;
    std::uint32_t pathPointsFirst;
    std::uint32_t pathPieces;
    std::uint32_t pathInverseFirst;
    std::uint32_t capsFirst;
    std::uint32_t capCount;
    std::uint32_t flags;
    std::uint32_t coverMapsFirst;
    std::uint32_t coverMapCount;
    std::uint32_t unused[2];
};

static_assert(sizeof(GpuPlantingMapsHeader) == 96, "GpuPlantingMapsHeader must match planting_cells.glsl's PlantingMaps");

// The maps' buffer: the header's words, then the maps'.
std::vector<std::uint32_t> packPlantingMaps(const PlantingMaps& maps);

// The maps back from their buffer (for the tests: every map where packPlantingMaps put it).
PlantingMaps unpackPlantingMaps(std::span<const std::uint32_t> words);

// One layer (PlantingLayerRecord), its keep-aways' reaches made on the CPU, its kinds' sizes in full tables; sharedKinds
// 1 where kinds are chosen by kindShares (unused shares 2, past every draw), 0 where alike; coverMap a cover layer's
// map (-1 none).
struct GpuPlantingLayer {
    float sharePerWeight[3];
    float gapStep;
    float nearPath;
    float farPath;
    float farShare;
    float inversePathSpan;
    float footprint;
    float jitterMetres;
    float pathClearance;
    std::int32_t under;
    std::uint32_t seed[2];
    std::int32_t kinds;
    std::uint32_t keepAwayCount;
    float kindShares[4];
    float leastScale;
    float scaleRange;
    float footRingPerScale;
    float footSink;
    float spacingShare;
    std::int32_t spacingRings;
    std::uint32_t sizes;
    std::uint32_t sharedKinds;
    std::int32_t keepLayer[PLANTING_MOST_KEEP_AWAYS];
    float keepRadius[PLANTING_MOST_KEEP_AWAYS];
    float keepReach[PLANTING_MOST_KEEP_AWAYS];
    std::uint32_t keepCrown[PLANTING_MOST_KEEP_AWAYS];
    float reachPerScale[PLANTING_MOST_KINDS];
    float centrePerScale[PLANTING_MOST_KINDS];
    std::int32_t coverMap;
    std::uint32_t unused[3];
};

static_assert(sizeof(GpuPlantingLayer) == 448, "GpuPlantingLayer must match planting_cells.glsl's PlantingLayer");

// Throws std::invalid_argument for layers plantLayers refuses or a layer keeping away from more than plantingMostKeepAways.
std::vector<GpuPlantingLayer> packPlantingLayers(const std::vector<PlantingLayerRecord>& layers);

/*
* One layer of one cell made this frame: its candidates' lattice, its standing lattice (kept plants), its cell's box's
* lattice and the box, where its candidates start among the frame's, where its plants go among the slots' and its count's
* place (slot times the layers plus the layer), and its plants' room.
*/
struct GpuPlantingJob {
    std::int32_t candidates[4];
    std::int32_t standing[4];
    std::int32_t box[4];
    float bounds[4];
    std::uint32_t candidateFirst;
    std::uint32_t plantFirst;
    std::uint32_t countPlace;
    std::uint32_t capacity;
};

static_assert(sizeof(GpuPlantingJob) == 80, "GpuPlantingJob must match planting_cells.glsl's PlantingJob");

/*
* The frame's jobs: cell by cell in the order made, a job a layer in each (cell * layers + layer). Throws
* std::logic_error where a lattice outgrows the layout's room (it never should: plantingSlotLayout's bound).
*/
std::vector<GpuPlantingJob> packPlantingJobs(const std::vector<PlantingLayerRecord>& layers, const PlantingSlotLayout& layout,
                                             std::span<const PlantingCellMade> made, float cellMetres);

// One candidate as the passes keep it: foot and scale; turn, pick, reach, threshold; kind, whether it is one, whether kept.
struct GpuPlantingCandidate {
    float footScale[4];
    float turnPickReachThreshold[4];
    std::uint32_t kind;
    std::uint32_t candidate;
    std::uint32_t kept;
    std::uint32_t unused;
};

static_assert(sizeof(GpuPlantingCandidate) == 48, "GpuPlantingCandidate must match planting_cells.glsl's PlantingCandidate");

// One placed plant in a slot (32 B, stage 2b2's record a plant).
struct GpuPlacedPlant {
    float foot[3];
    float scale;
    float turnRadians;
    float pick;
    std::uint32_t kind;
    float threshold;
};

static_assert(sizeof(GpuPlacedPlant) == 32, "GpuPlacedPlant must match planting_cells.glsl's PlacedPlant");

GpuPlacedPlant packPlacedPlant(const PlantedPlant& plant);

// One layer's comparison over every cell compared: plants equal bit for bit, the CPU's the GPU lacks, the GPU's the
// CPU lacks, and cells whose plants agree but in another order.
struct PlantingLayerComparison {
    std::size_t matching = 0;
    std::size_t missing = 0;
    std::size_t extra = 0;
    std::size_t reordered = 0;
};

struct PlantingComparison {
    std::size_t cells = 0;
    std::vector<PlantingLayerComparison> layers;
    std::array<std::uint32_t, PLANTING_PROBLEMS> problems{};
};

/*
* One cell's plants, a layer each, as the CPU planted them and as the GPU did, added to a comparison (its layers grown
* to fit).
*/
void comparePlantingCell(PlantingComparison& comparison, const std::vector<std::vector<PlantedPlant>>& cpu,
                         const std::vector<std::vector<GpuPlacedPlant>>& gpu);

// The CPU's plants of a planting by cell (each plant in the cell whose box holds its foot), in their order.
std::map<PlantingCell, std::vector<std::vector<PlantedPlant>>> plantingByCell(const std::vector<std::vector<PlantedPlant>>& planted,
                                                                               float cellMetres);

// The box round a set of cells.
PlantingBox plantingCellsBox(std::span<const PlantingCell> cells, float cellMetres);

// placement-compare's reply: the cells, each layer's counts, the problems, and whether they agree.
std::string describePlantingComparison(const PlantingComparison& comparison);
