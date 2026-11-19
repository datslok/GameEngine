#pragma once

#include "core/job_system.h"
#include "core/settings_visitor.h"
#include "games/forest/bastion.h"
#include "games/forest/valley_shapes.h"
#include "math/vec2.h"
#include "math/vec3.h"

#include <array>
#include <cstdint>
#include <vector>

/*
* The valley's planting maps (ROADMAP phase 18, the rest of the valley's stage 1, docs/research/VALLEY.md): what every
* layer of plants is planted from, in place of darts thrown one by one. Horizon Zero Dawn's way: only maps are stored;
* each layer's density at a place is a small function of them, and an ordered dither turns the density into plants, the
* same at a place whatever order or wherever it is asked, so any cell of the valley can be planted alone (on the CPU now,
* on the GPU round the camera in stage 2).
*
* - Ecotopes: how much of a place is floor jungle, wall slope, rim forest or bare (the clearing, the meadow, cliffs), from
*   the land's shape across the valley and its slope. A layer's density is its plants a hectare in each, weighted.
* - Clearance: how far a place is from the nearest obstacle's edge (pillars, the Ring's feet, the towers), a distance
*   field, so one map serves every layer however far its crowns reach.
* - Canopy caps: the lowest a sight line passes over a place, for each layer's reach, so crowns stay under the lines from
*   the designed viewpoints to the Monolith and the Ring (Ghost of Tsushima's cleared landmarks' views).
* - Gaps: fractal noise, where the big trees thin out and palms and the understorey take the light (treefall gaps).
*/

// A line of sight from somewhere walked to a landmark.
struct SightLine {
    Vec3 from;
    Vec3 to;
};

enum class Ecotope {
    Floor,
    Wall,
    Rim,
    Bare
};

inline constexpr int ecotopeCount = 4;

// The share of a place each ecotope has, summing to 1.
using EcotopeWeights = std::array<float, ecotopeCount>;

/*
* The maps (F5 Forest, Planting; read at start-up). Their extent: within halfWidth of the valley's axis, from northmost to
* southmost, the floor, walls and rim (not the mountains beyond); texelMetres a texel.
*/
struct ForestPlantingSettings {
    float texelMetres = 2.0f;
    float halfWidth = 450.0f;
    float northmost = -5000.0f;
    float southmost = 800.0f;

    // How far either side of where the floor meets the wall, and the wall the rim, one ecotope turns to the next.
    float ecotopeBlendMetres = 30.0f;

    // The ground's slope (rise over run) at which it starts and finishes turning bare: cliffs hold no forest.
    float bareSlopeFrom = 1.2f;
    float bareSlopeTo = 2.0f;

    // How far round the clearing and the meadow their edge of bare ground takes to fill with forest.
    float clearingEdgeMetres = 10.0f;

    // The gaps: fractal noise (-1..1) over gapMetres, a place a gap from gapFrom, fully one from gapTo.
    float gapMetres = 70.0f;
    float gapFrom = 0.3f;
    float gapTo = 0.5f;

    // How far under a sight line every crown stays.
    float corridorClearance = 2.0f;
};

void visit(ForestPlantingSettings& settings, SettingsVisitor& visitor);

/*
* How one layer of plants is planted from the maps (in each species' settings): its plants a hectare asked in each
* ecotope (the bare one has none; fewer stand, as crowding, trunks, the path and the corridors drop some), its dither
* cells' size (one plant a cell at most) and how far a plant strays from its cell's middle (a share of the cell), its
* density in a full gap as a share of its own (below 1 thins, above thickens), and how it keeps to the path: none within
* pathClearance of its centre line, all of the density within nearPathMetres, farShare of it past farPathMetres, a
* straight blend between.
*/
struct PlantingLayer {
    float floorPerHectare = 0.0f;
    float wallPerHectare = 0.0f;
    float rimPerHectare = 0.0f;
    float footprintMetres = 10.0f;
    float jitterShare = 0.8f;
    float gapShare = 1.0f;
    float pathClearance = 0.0f;
    float nearPathMetres = 0.0f;
    float farPathMetres = 0.0f;
    float farShare = 1.0f;
};

// The layer's fields, named once for every species.
void visit(PlantingLayer& layer, SettingsVisitor& visitor);

// Something no plant's reach may cross: a disc on the ground round centre.
struct ValleyObstacle {
    Vec2 centre;
    float radius = 0.0f;
};

// How far a pillar's footprint reaches past its radius (pillars bulge and lean).
inline constexpr float pillarBulgeShare = 1.3f;

// How far round the Ring's feet, where its tube meets the ground, as a share of the tube's radius.
inline constexpr float ringFootShare = 1.5f;

// The obstacles: the pillars, the Ring's two feet, and each tower's footprint (its widest section's half diagonal).
std::vector<ValleyObstacle> forestObstacles(const std::vector<PillarPlacement>& pillars, const std::vector<BastionShape>& towers);

// The farthest the clearance map tells apart, in metres: beyond, a place is simply far from every obstacle.
inline constexpr float farthestClearance = 60.0f;

// The canopy caps store heights in decimetres above -capFloorMetres; noCap where no line passes.
inline constexpr float capFloorMetres = 1000.0f;
inline constexpr std::uint16_t noCap = 0xffff;

/*
* One canopy cap map: over the lines' footprint grown by the largest reach, on a grid square to the world's axes, each
* texel the lowest any line passes within reach of its middle and half a texel's diagonal more, so of anywhere in it
* (rounded down, in decimetres above -capFloorMetres; noCap where none does).
*/
struct CanopyCap {
    float reach = 0.0f;
    Vec2 origin;
    int columns = 0;
    int rows = 0;
    std::vector<std::uint16_t> heights;
};

/*
* The maps over the extent, on a grid across the valley's axis: column c's middle at u = -halfWidth + (c + 0.5) texel
* across the axis (x = valleyAxis(z) + u), row r's at z = northmost + (r + 0.5) texel. Each ecotope's weight in 0..255
* (summing to 255), each clearance in decimetres (rounded down, at most farthestClearance), and one canopy cap a reach.
* texelReach is the farthest a place lies from the middle of its texel, in metres across the ground (the axis winds, so
* more than half a texel's diagonal): what the clearance read at a place gives away to stay a lower bound.
*/
struct ValleyMaps {
    ForestPlantingSettings settings;
    int columns = 0;
    int rows = 0;
    float texelReach = 0.0f;
    std::vector<std::array<std::uint8_t, ecotopeCount>> ecotopes;
    std::vector<std::uint16_t> clearance;
    std::vector<CanopyCap> caps;
};

/*
* The ecotopes' weights at a place from the land alone (before the maps quantise them): across the axis floor, then
* wall, then rim (each blending into the next over ecotopeBlendMetres either side of the land's own edges), then bare by
* the clearing, the meadow and the slope. Outside the extent, all bare.
*/
EcotopeWeights landEcotopes(const ForestPlantingSettings& settings, float x, float z, float slope);

/*
* The maps built on every core (each row its own job, so the same whatever the order): the ecotopes from the land's height
* and slope at each texel, the clearance from the obstacles, one canopy cap for each reach in capReaches from the sight
* lines.
*/
ValleyMaps buildValleyMaps(const ForestPlantingSettings& settings, const std::vector<ValleyObstacle>& obstacles,
                           const std::vector<SightLine>& lines, const std::vector<float>& capReaches, JobSystem& jobs);

// The ecotopes at a place, blended between the four texels round it; all bare outside the extent.
EcotopeWeights valleyEcotopes(const ValleyMaps& maps, float x, float z);

// A lower bound on how far a place is from the nearest obstacle's edge (farthestClearance or more: far from all).
float valleyClearance(const ValleyMaps& maps, float x, float z);

/*
* The lowest a sight line passes within reach of a place (a lower bound, in metres; very high where none does), from the
* cap of the smallest reach at least reach. Throws when no cap reaches that far (the maps were built without it).
*/
float valleyCap(const ValleyMaps& maps, float reach, float x, float z);

// How much of a gap a place is (0..1).
float valleyGap(const ForestPlantingSettings& settings, float x, float z);

// A layer's share of its density at a distance from the path's centre line: 1 within nearPathMetres, farShare past
// farPathMetres, a straight blend between (its clearance is kept at each plant's own place, by ditheredCells).
float pathDensityShare(const PlantingLayer& layer, float pathDistance);

/*
* The share of a layer's dither cells planted at a place (0..1): its plants a hectare in each ecotope weighted by the
* ecotope's share, times a cell's area, thinned or thickened in the gaps and by the path.
*/
float layerShare(const PlantingLayer& layer, const ValleyMaps& maps, float x, float z);

/*
* The ordered dither's threshold of a cell of a lattice (0..1): Roberts' R2 sequence over the cells, fract(column a1 +
* row a2) with a1, a2 from the plastic number, in 32-bit whole numbers so every machine (and a shader) gets the same.
* Any range of thresholds is spread evenly over the cells, so the cells under a share are about that share of them and
* far apart where it is small.
*/
float ditherThreshold(int column, int row);

// The region a layer is planted in now: within spread of the valley's axis, from northmost to southmost.
struct PlantingRegion {
    float northmost = 0.0f;
    float southmost = 0.0f;
    float spread = 0.0f;
};

/*
* A dither cell planted: its lattice cell, its plant's place (x, z), its threshold, and the first of its own random
* numbers left for its plant (forestUnitRandom(seed, firstDraw + k); the first two placed it).
*/
struct DitheredCell {
    int column = 0;
    int row = 0;
    Vec2 place;
    float threshold = 0.0f;
    std::uint64_t firstDraw = 0;
};

inline constexpr std::uint64_t ditherDrawsUsed = 2;
inline constexpr std::uint64_t ditherDrawsPerCell = 8;

/*
* A layer's cells planted over a region, in rows from the north: the lattice's cells footprintMetres square from the
* world's origin, each planted when its threshold is under the layer's share at the cell's middle, its plant jittered
* within the cell (jitterShare of it) by its own random numbers, kept when the plant is inside the region and clear of
* the path. under is a layer sharing the lattice (same footprint) planted first: this one takes the band of thresholds
* above its share, so the two never take the same cell. The same every run.
*/
std::vector<DitheredCell> ditheredCells(const PlantingLayer& layer, const ValleyMaps& maps, const PlantingRegion& region, std::uint64_t seed,
                                        const PlantingLayer* under = nullptr);

/*
* Whether a plant of reach (its sphere's radius, centred over place) with its top at topHeight fits: its reach clear of
* every obstacle, its top corridorClearance under every sight line within its reach.
*/
bool fitsTheMaps(const ValleyMaps& maps, const Vec2& place, float reach, float topHeight);
