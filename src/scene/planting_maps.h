#pragma once

#include "core/job_system.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/planting.h"
#include "scene/planting_shared.h"
#include "scene/terrain_heights.h"

#include <array>
#include <cstdint>
#include <vector>

/*
* The maps a stretch of land is planted from, and its layers planted from them a box at a time (ROADMAP phase 18, the
* valley's stage 2, docs/research/VALLEY.md, docs/architecture/planting.md). A game builds the maps (the forest's:
* games/forest/valley_maps.h); everything here reads them with planting.h's exact maths only, so the GPU, reading the
* same maps, plants the same plants. Any box gives the plants a planting of everything would put there.
*/

/*
* One canopy cap: on a grid square to the world's axes from origin, each texel the lowest a sight line passes within
* reach of anywhere in it, decimetres above -floorMetres (noCap where none does).
*/
struct PlantingCap {
    float reach = 0.0f;
    Vec2 origin;
    int columns = 0;
    int rows = 0;
    std::vector<std::uint16_t> heights;
};

/*
* A cover layer's map (ROADMAP phase 18, d3): a byte a cell of the layer's own lattice, from firstColumn and firstRow on,
* rows from the north: 0 plants nothing, else plantingCoverValue's kind and quarter turns. Baked by the game from what
* covers the ground there (the forest's paint), so the cell's plant needs no dither, share or random kind.
*/
struct PlantingCoverMap {
    int firstColumn = 0;
    int firstRow = 0;
    int columns = 0;
    int rows = 0;
    std::vector<std::uint8_t> values;
};

// A cover map's byte at a lattice cell (0 outside it).
int plantingCoverAt(const PlantingCoverMap& map, int column, int row);

inline constexpr std::uint16_t plantingNoCap = PLANTING_NO_CAP;
inline constexpr float plantingCapFloorMetres = static_cast<float>(PLANTING_CAP_FLOOR_METRES);
inline constexpr float plantingUncapped = static_cast<float>(PLANTING_UNCAPPED);

// The farthest the clearance map tells apart, in metres: beyond, a place is simply far from every obstacle.
inline constexpr float plantingFarthestClearance = static_cast<float>(PLANTING_FARTHEST_CLEARANCE);

/*
* The maps on a grid across a winding axis: row r's middle at z = northmost + (r + 0.5) texel, where the axis is at
* axis[r] (read between rows' middles in a straight line); column c's middle at axis + (c + 0.5) texel - halfWidth.
* - ecotopes: four weights 0..255 summing to 255 (floor, wall, rim, bare);
* - gaps: how much of a gap 0..255;
* - path: the distance to the path's centre line in centimetres (for its density's blend; clearing it is tested against
*   pathPoints, its straight pieces, pathInverseLengths one over each piece's length squared);
* - clearance: decimetres to the nearest obstacle's edge at the texel's middle (at most the farthest told apart), less
*   texelReach when read (the farthest a place lies from its texel's middle), so a lower bound;
* - caps: one a reach, smallest first; corridorClearance how far under a cap every top stays;
* - coverMaps: the cover layers' (PlantingLayerRecord::coverMap), on their own lattices.
* inverseTexel is 1 / texelMetres, made once on the CPU so both sides multiply by the same number.
*/
struct PlantingMaps {
    int columns = 0;
    int rows = 0;
    float texelMetres = 2.0f;
    float inverseTexel = 0.5f;
    float halfWidth = 0.0f;
    float northmost = 0.0f;
    float texelReach = 0.0f;
    float corridorClearance = 0.0f;
    std::vector<float> axis;
    std::vector<std::array<std::uint8_t, 4>> ecotopes;
    std::vector<std::uint8_t> gaps;
    std::vector<std::uint16_t> path;
    std::vector<Vec2> pathPoints;
    std::vector<float> pathInverseLengths;
    std::vector<std::uint16_t> clearance;
    std::vector<PlantingCap> caps;
    std::vector<PlantingCoverMap> coverMaps;
};

// The axis's x at a place's z, blended between the rows' middles (held past the first and last).
float plantingAxisAt(const PlantingMaps& maps, float z);

// A stretch of the planted land: from northmost to southmost, within spread of the maps' axis either way.
struct PlantedRegion {
    float northmost = 0.0f;
    float southmost = 0.0f;
    float spread = 0.0f;
};

// Whether a place's foot is in the region, the axis read from the maps.
bool insidePlantedRegion(const PlantingMaps& maps, const PlantedRegion& region, const Vec3& foot);

// The place on the grid across the axis: its column and row coordinates, in texels from the grid's corner.
Vec2 plantingGridPlace(const PlantingMaps& maps, float x, float z);

// The floor's, wall's, rim's and bare's weights at a place (0..255 each), blended between its four texels; all bare
// outside the maps.
std::array<float, 4> plantingEcotopes(const PlantingMaps& maps, float x, float z);

// How much of a gap a place is (0..255), blended; none outside the maps.
float plantingGap(const PlantingMaps& maps, float x, float z);

// The distance from a place to the path's centre line in metres, blended; the farthest stored outside the maps.
float plantingPathDistanceAt(const PlantingMaps& maps, float x, float z);

// Whether a place is clearance or more from every piece of the path (exactly, not from the map).
bool plantingClearOfThePath(const PlantingMaps& maps, const Vec2& place, float clearance);

// A lower bound on how far a place is from the nearest obstacle's edge (the farthest told apart, or more, outside).
float plantingClearance(const PlantingMaps& maps, float x, float z);

/*
* The lowest a sight line passes within reach of a place (a lower bound; very high where none does), from the cap of the
* smallest reach at least reach. Throws when no cap reaches that far.
*/
float plantingCap(const PlantingMaps& maps, float reach, float x, float z);

// Whether a plant of reach centred over place, its top at top, fits the maps (planting.h's plantingFits).
bool plantingFitsTheMaps(const PlantingMaps& maps, const Vec2& place, float reach, float top);

// The GPU's sets of cells round the camera (planting_cells.h), coarsest first: each plants the layers naming it.
inline constexpr int plantingCoarseCells = 0;
inline constexpr int plantingMiddleCells = 1;
inline constexpr int plantingFineCells = 2;
inline constexpr int plantingCellSetCount = 3;

// Plants of another layer a plant keeps away from: their trunks within radius, or (crown) their own reach.
struct PlantingKeepAway {
    int layer = 0;
    float radius = 0.0f;
    bool crown = false;
};

/*
* One layer of plants, as planted (a game's settings turned into what the maths reads; the GPU's copy of it in stage 2):
* - sharePerWeight: its share of a dither cell a unit of the floor's, wall's and rim's weight (plants a hectare times a
*   cell's area over 255); gapStep: (its share in a full gap - 1) / 255; the path's blend (plantingPathShare);
* - footprint: its dither cells' side; jitterMetres how far a plant strays across its cell; pathClearance: none nearer;
* - seed: its own random numbers; under: a layer planted before on the same lattice, whose share this one plants above;
* - kinds: by cumulative shares (kindShares, the share up to each kind but the last), or alike when there are none; each
*   kind its reach and its sphere's centre's height a unit of scale (one entry: every kind the same);
* - scale from leastScale to leastScale + scaleRange; footRingPerScale: the trunk's ring the lowest ground is read on (0:
*   the ground at the foot alone), footSink how far it is sunk;
* - spacingShare: a plant gives way to an earlier one of its own layer closer than this share of the larger reach (0:
*   none); keepAway: earlier layers' plants it keeps clear of;
* - coverMap: a cover layer's map in the maps' coverMaps (-1: none). A cover layer plants exactly the cells its map
*   names, at the cell's middle, its kind and turn from the byte (plantingCoverKind, plantingCoverTurn), its scale and
*   pick drawn as any layer's, on the ground at its middle; the dither, shares, path, keep-aways and fits are not asked;
* - cellSet: which of the GPU's sets of cells plants it (plantingCoarseCells to plantingFineCells, planting_cells.h); the
*   CPU's plantings ignore it.
*/
struct PlantingLayerRecord {
    std::array<float, 3> sharePerWeight{};
    float gapStep = 0.0f;
    float nearPath = 0.0f;
    float farPath = 0.0f;
    float farShare = 1.0f;
    float inversePathSpan = 0.0f;
    float footprint = 10.0f;
    float jitterMetres = 0.0f;
    float pathClearance = 0.0f;
    std::uint64_t seed = 0;
    int under = -1;
    int kinds = 1;
    std::vector<float> kindShares;
    std::vector<float> reachPerScale{1.0f};
    std::vector<float> centrePerScale{0.0f};
    float leastScale = 1.0f;
    float scaleRange = 0.0f;
    float footRingPerScale = 0.0f;
    float footSink = 0.0f;
    float spacingShare = 0.0f;
    std::vector<PlantingKeepAway> keepAway;
    int coverMap = -1;
    int cellSet = plantingCoarseCells;
};

/*
* A layer's density as asked: plants a hectare on the floor, wall and rim, its cells' side, how far a plant strays (a
* share of its cell), its density in a full gap (a share of its own), and the path's clearance and blend. Turned into a
* record's share fields (the rest left as they are).
*/
struct PlantingDensity {
    std::array<float, 3> perHectare{};
    float footprintMetres = 10.0f;
    float jitterShare = 0.8f;
    float gapShare = 1.0f;
    float pathClearance = 0.0f;
    float nearPathMetres = 0.0f;
    float farPathMetres = 0.0f;
    float farShare = 1.0f;
};

void setPlantingDensity(PlantingLayerRecord& record, const PlantingDensity& density);

// The largest reach any of a layer's plants has.
float plantingLayerReach(const PlantingLayerRecord& layer);

// The share of a layer's dither cells planted at a place (0..1).
float plantingLayerShare(const PlantingLayerRecord& layer, const PlantingMaps& maps, float x, float z);

/*
* A plant: its foot (on the ground, sunk), scale, turn, kind, its own random pick (a variant within its kind), its reach
* (its sphere's radius), and its dither cell and threshold.
*/
struct PlantedPlant {
    Vec3 foot;
    float scale = 1.0f;
    float turnRadians = 0.0f;
    int kind = 0;
    float pick = 0.0f;
    float reach = 0.0f;
    float threshold = 0.0f;
    int column = 0;
    int row = 0;
};

// A box on the ground: x from west up to east, z from north up to south (each side's start in, its end out).
struct PlantingBox {
    float west = 0.0f;
    float east = 0.0f;
    float north = 0.0f;
    float south = 0.0f;
};

/*
* A layer's dither cells over a box, a cell more each way (columns along x, rows along z, from the first): those whose
* plant could stand in it, since a plant strays at most a cell from its own.
*/
struct PlantingLattice {
    int firstColumn = 0;
    int firstRow = 0;
    int columns = 0;
    int rows = 0;

    bool holds(int column, int row) const;
    std::size_t index(int column, int row) const;
    std::size_t count() const;
};

PlantingLattice plantingLatticeOver(const PlantingBox& box, float footprint);

// The most cells plantingLatticeOver gives any box of this size, wherever it lies (a cell more for rounding).
std::size_t plantingLatticeCapacity(float width, float height, float footprint);

// How far round a plant its own layer's neighbours can make it give way, in lattice cells (0 without spacing).
int plantingSpacingRings(const PlantingLayerRecord& layer);

// How far a keep-away reaches: the other layer's largest reach for a crown, else its radius.
float plantingKeepAwayReach(const PlantingKeepAway& keep, const std::vector<PlantingLayerRecord>& layers);

/*
* What planting a box plants of each layer: standing, the box its kept plants are needed over (the asked box joined
* with what every later layer reads of it); candidates, that box grown by its spacing's rings and a cell, the
* candidates its spacing reads. plantLayers plants these, and the GPU's cells the same (render/gpu/planting_cells.h).
*/
struct PlantingLayerBoxes {
    PlantingBox standing;
    PlantingBox candidates;
};

std::vector<PlantingLayerBoxes> plantingLayerBoxes(const std::vector<PlantingLayerRecord>& layers, const PlantingBox& box);

// Throws std::invalid_argument for a layer reading itself or a later one, sharing a lattice of another footprint, or
// with its kinds, sizes or footprint out of range.
void checkPlantingLayers(const std::vector<PlantingLayerRecord>& layers);

// Throws std::invalid_argument for a cover layer whose map the maps lack, is the wrong size, or names a kind it lacks.
void checkPlantingCover(const std::vector<PlantingLayerRecord>& layers, const PlantingMaps& maps);

/*
* Every layer's plants standing in a box, in order (each layer's cells by rows from the north), layers in order: each
* reads only the layers before it (keepAway, under). Each layer is planted over the box grown by what the later layers
* and its own spacing read, so the box's plants are exactly those planting a larger box (or everything) puts in it.
* Rows on the jobs. Throws for a layer reading itself or a later one, or sharing a lattice of another footprint.
*/
std::vector<std::vector<PlantedPlant>> plantLayers(const std::vector<PlantingLayerRecord>& layers, const PlantingMaps& maps,
                                                   const TerrainHeights& heights, const PlantingBox& box, JobSystem& jobs);
