#pragma once

#include "core/job_system.h"
#include "games/forest/forest_trees.h"
#include "games/forest/palms.h"
#include "games/forest/understorey.h"
#include "games/forest/valley_maps.h"
#include "scene/planting_maps.h"
#include "scene/terrain_heights.h"

#include <vector>

/*
* The forest's layers planted from the valley's maps (scene/planting_maps.h; ROADMAP phase 18, the valley's stage 2):
* the emergents, the canopy trees on their lattice above their share and clear of their crowns, the mid layer clear of
* both's trunks, then the understorey and the palms clear of every trunk. Any box of the valley planted alone gives the
* plants planting it all would; the GPU will plant the same from the same records.
*/

// How far round each trunk the understorey and the palms keep clear (plus their own trunkClearance): the big trees'
// buttresses, the mid layer's trunk.
inline constexpr float bigTrunkReach = 2.0f;
inline constexpr float midTrunkReach = 0.4f;

// The species' settings the layers come from.
struct ForestSpecies {
    const ForestTreeSettings& emergent;
    const ForestTreeSettings& canopy;
    const ForestTreeSettings& mid;
    const ForestUnderstoreySettings& understorey;
    const ForestPalmSettings& palms;
};

// The layers in the order they are planted, each its place in forestPlantingLayers.
enum class ForestLayer {
    Emergent,
    Canopy,
    Mid,
    Understorey,
    Palms
};

inline constexpr int forestLayerCount = 5;

/*
* The layers, in ForestLayer's order: the canopy shares the emergents' lattice while their footprints match, and keeps
* its trunks outside their crowns; the mid layer keeps midTrunkClearance from both's trunks; the understorey and the
* palms keep their trunkClearance past each big trunk's bigTrunkReach and each mid trunk's midTrunkReach.
*/
std::vector<PlantingLayerRecord> forestPlantingLayers(const ForestSpecies& species);

// Every layer's plants as the forest places them.
struct ForestPlanting {
    std::vector<TreePlacement> emergents;
    std::vector<TreePlacement> canopy;
    std::vector<TreePlacement> mid;
    std::vector<UnderstoreyPlacement> understorey;
    std::vector<PalmPlacement> palms;
};

// The layers' plants standing in a box (plantLayers), as the forest's placements.
ForestPlanting plantTheForest(const ForestSpecies& species, const ValleyMaps& maps, const TerrainHeights& heights, const PlantingBox& box,
                              JobSystem& jobs);

// The patch planted as entities until the cells round the camera take over (stage 2b): the box round forestPatch, the
// plants inside it kept.
ForestPlanting plantThePatch(const ForestSpecies& species, const ValleyMaps& maps, const TerrainHeights& heights, JobSystem& jobs);
