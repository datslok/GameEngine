#pragma once

#include "core/job_system.h"
#include "core/settings_visitor.h"
#include "games/forest/bastion.h"
#include "games/forest/valley_shapes.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/planting_maps.h"

#include <array>
#include <cstdint>
#include <vector>

/*
* The valley's planting maps (ROADMAP phase 18, the rest of the valley's stage 1, docs/research/VALLEY.md): what every
* layer of plants is planted from, in place of darts thrown one by one. Horizon Zero Dawn's way: only maps are stored;
* each layer's density at a place is a small function of them, and an ordered dither turns the density into plants, the
* same at a place whatever order or wherever it is asked, so any cell of the valley can be planted alone (scene/planting_maps.h,
* the CPU's and the GPU's alike).
*
* - Ecotopes: how much of a place is floor jungle, wall slope, rim forest or bare (the clearing, the meadow, cliffs), from
*   the land's shape across the valley and its slope. A layer's density is its plants a hectare in each, weighted.
* - Clearance: how far a place is from the nearest obstacle's edge (pillars, the Ring's feet, the towers), a distance
*   field, so one map serves every layer however far its crowns reach.
* - Canopy caps: the lowest a sight line passes over a place, for each layer's reach, so crowns stay under the lines from
*   the designed viewpoints to the Monolith and the Ring (Ghost of Tsushima's cleared landmarks' views).
* - Gaps: fractal noise, where the big trees thin out and palms and the understorey take the light (treefall gaps).
* - The path: its distance, which a layer's density blends by, and its straight pieces, which a layer clears exactly.
* - The axis: its x at each row's middle, read between them in straight lines (no sine on the GPU).
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

/*
* The valley's maps (scene/planting_maps.h's PlantingMaps, read there) and the settings they were built with: on a grid
* across the valley's axis, the axis at each row's middle; the ecotopes; the gaps; the path's distance and pieces; the
* clearance from the obstacles; one canopy cap a reach from the sight lines.
*/
struct ValleyMaps {
    ForestPlantingSettings settings;
    PlantingMaps planting;
};

/*
* The ecotopes' weights at a place from the land alone (before the maps quantise them): across the axis floor, then
* wall, then rim (each blending into the next over ecotopeBlendMetres either side of the land's own edges), then bare by
* the clearing, the meadow and the slope. Outside the extent, all bare.
*/
EcotopeWeights landEcotopes(const ForestPlantingSettings& settings, float x, float z, float slope);

/*
* The maps built on every core (each row its own job, so the same whatever the order): the axis at each row, the gaps
* from their noise and the path's distance at each texel, the ecotopes from the land's height and slope, the
* clearance from the obstacles, one canopy cap for each reach in capReaches from the sight lines.
*/
ValleyMaps buildValleyMaps(const ForestPlantingSettings& settings, const std::vector<ValleyObstacle>& obstacles,
                           const std::vector<SightLine>& lines, const std::vector<float>& capReaches, JobSystem& jobs);

// The ecotopes at a place, blended between the four texels round it, as shares summing to 1; all bare outside the maps.
EcotopeWeights valleyEcotopes(const ValleyMaps& maps, float x, float z);

// How much of a gap a place is (0..1), from the noise the gap map is built from.
float valleyGap(const ForestPlantingSettings& settings, float x, float z);

// A layer's density for the planting (scene/planting_maps.h).
PlantingDensity plantingDensity(const PlantingLayer& layer);

// The region a layer is planted in now: within spread of the valley's axis, from northmost to southmost.
struct PlantingRegion {
    float northmost = 0.0f;
    float southmost = 0.0f;
    float spread = 0.0f;
};

// The box round a region on the ground (the axis winds, so wider than 2 spread), and whether a place is in the region.
PlantingBox regionBox(const PlantingRegion& region);
bool insideRegion(const PlantingRegion& region, const Vec3& foot);
