#pragma once

#include "core/job_system.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/planting.h"
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

inline constexpr std::uint16_t plantingNoCap = 0xffff;
inline constexpr float plantingCapFloorMetres = 1000.0f;
inline constexpr float plantingUncapped = 1e9f;

// The farthest the clearance map tells apart, in metres: beyond, a place is simply far from every obstacle.
inline constexpr float plantingFarthestClearance = 60.0f;

/*
* The maps on a grid across a winding axis: row r's middle at z = northmost + (r + 0.5) texel, where the axis is at
* axis[r] (read between rows' middles in a straight line); column c's middle at axis + (c + 0.5) texel - halfWidth.
* - ecotopes: four weights 0..255 summing to 255 (floor, wall, rim, bare);
* - gaps: how much of a gap 0..255;
* - path: the distance to the path's centre line in centimetres (for its density's blend; clearing it is tested against
*   pathPoints, its straight pieces, pathInverseLengths one over each piece's length squared);
* - clearance: decimetres to the nearest obstacle's edge at the texel's middle (at most the farthest told apart), less
*   texelReach when read (the farthest a place lies from its texel's middle), so a lower bound;
* - caps: one a reach, smallest first; corridorClearance how far under a cap every top stays.
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
};

// The axis's x at a place's z, blended between the rows' middles (held past the first and last).
float plantingAxisAt(const PlantingMaps& maps, float z);

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
*   none); keepAway: earlier layers' plants it keeps clear of.
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
* Every layer's plants standing in a box, in order (each layer's cells by rows from the north), layers in order: each
* reads only the layers before it (keepAway, under). Each layer is planted over the box grown by what the later layers
* and its own spacing read, so the box's plants are exactly those planting a larger box (or everything) puts in it.
* Rows on the jobs. Throws for a layer reading itself or a later one, or sharing a lattice of another footprint.
*/
std::vector<std::vector<PlantedPlant>> plantLayers(const std::vector<PlantingLayerRecord>& layers, const PlantingMaps& maps,
                                                   const TerrainHeights& heights, const PlantingBox& box, JobSystem& jobs);
