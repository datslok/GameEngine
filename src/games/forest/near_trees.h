#pragma once

#include "core/job_system.h"
#include "core/settings_visitor.h"
#include "games/forest/forest_planting.h"
#include "games/forest/forest_trees.h"
#include "games/forest/lianas.h"
#include "scene/planting_cells.h"
#include "scene/planting_maps.h"
#include "scene/terrain_heights.h"

#include <cstdint>
#include <optional>
#include <vector>

/*
* The big trees the CPU keeps near the player (ROADMAP phase 18, d2; docs/architecture/forest-trees.md): planted by the
* same maths as the GPU's cells (plantLayers), a square cell of land at a time, with the lianas hung from their crowns.
* A cell's trees and lianas are the same whichever cells are made with it, so cells come and go one by one.
*/

// The near cells (F5 Forest, Near trees): their side in metres (start-up; the lianas do not depend on it).
struct ForestNearTreeSettings {
    float cellMetres = 64.0f;
};

void visit(ForestNearTreeSettings& settings, SettingsVisitor& visitor);

// A big tree (emergent or canopy) and the dither cell of its layer it grew from, which name it.
struct NearTree {
    TreePlacement placement;
    ForestLayer layer = ForestLayer::Canopy;
    int column = 0;
    int row = 0;
};

// A tree's identity: its layer and dither cell, the same whichever box planted it.
std::uint64_t nearTreeIdentity(const NearTree& tree);

// The big trees standing in a box: the emergents, then the canopy trees, each layer by rows from the north (plantLayers).
std::vector<NearTree> plantBigTrees(const ForestSpecies& species, const PlantingMaps& maps, const TerrainHeights& heights, const PlantingBox& box,
                                    JobSystem& jobs);

// The cells a box touches, by rows from the north then columns.
std::vector<PlantingCell> nearCellsOver(const PlantingBox& box, float cellMetres);

// What a near cell is made from: the species, maps and land the trees stand on, the region drawn (none: everywhere),
// and how the lianas hang.
struct NearTreeSources {
    const ForestSpecies& species;
    const PlantingMaps& maps;
    const TerrainHeights& heights;
    std::optional<PlantedRegion> region;
    const ForestLianaSettings& lianas;
    std::uint64_t lianaSeed = 0;
};

/*
* A cell's trees and lianas: the big trees inside the region whose feet the cell holds, and those within the lianas'
* neighbourSpan of it they may loop into, a crown each (trees[i] is crowns[i]'s tree; carries marks the cell's own),
* and the lianas the cell's own crowns hang (chooseLianas), their crowns indexing crowns.
*/
struct NearTreeCell {
    std::vector<NearTree> trees;
    std::vector<LianaCrown> crowns;
    std::vector<Liana> lianas;
};

NearTreeCell makeNearTreeCell(const PlantingCell& cell, float cellMetres, const NearTreeSources& sources, JobSystem& jobs);
