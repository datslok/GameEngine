#include "games/forest/near_trees.h"
#include "games/forest/forest_terrain.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

void visit(ForestNearTreeSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Cell size", settings.cellMetres, {.min = 8.0f, .max = 512.0f, .sliderMax = 256.0f, .unit = "m", .applies = FieldApplies::AtStartup});
}

/*
* The layer and the dither cell's column and row packed into one number: column and row spread by an odd multiplier
* (so no two cells meet), the layer added, which a multiple of the multiplier can never cancel for the valley's cells.
*/
std::uint64_t nearTreeIdentity(const NearTree& tree) {
    const std::uint64_t cell = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tree.column)) << 32) | static_cast<std::uint32_t>(tree.row);
    return cell * 0x9E3779B97F4A7C15ull + static_cast<std::uint64_t>(tree.layer);
}

/*
* Only the emergents' and the canopy's layers: they read nothing after them, so planting the two alone stands the same
* trees as planting every layer (plantLayers' boxes), for a fraction of the work.
*/
std::vector<NearTree> plantBigTrees(const ForestSpecies& species, const PlantingMaps& maps, const TerrainHeights& heights, const PlantingBox& box,
                                    JobSystem& jobs) {
    std::vector<PlantingLayerRecord> layers = forestPlantingLayers(species);
    layers.resize(static_cast<std::size_t>(ForestLayer::Canopy) + 1);
    const std::vector<std::vector<PlantedPlant>> planted = plantLayers(layers, maps, heights, box, jobs);
    std::vector<NearTree> trees;

    for (const ForestLayer layer : {ForestLayer::Emergent, ForestLayer::Canopy}) {
        for (const PlantedPlant& plant : planted[static_cast<std::size_t>(layer)]) {
            trees.push_back(NearTree{TreePlacement{plant.foot, plant.scale, plant.turnRadians, plant.kind}, layer, plant.column, plant.row});
        }
    }

    return trees;
}

std::vector<PlantingCell> nearCellsOver(const PlantingBox& box, float cellMetres) {
    if (!(cellMetres > 0.0f)) {
        throw std::invalid_argument("nearCellsOver: a cell's side must be above 0");
    }

    // A box's end is out, so a box ending on a cell's edge does not touch the next.
    const int firstColumn = static_cast<int>(std::floor(box.west / cellMetres));
    const int lastColumn = static_cast<int>(std::ceil(box.east / cellMetres)) - 1;
    const int firstRow = static_cast<int>(std::floor(box.north / cellMetres));
    const int lastRow = static_cast<int>(std::ceil(box.south / cellMetres)) - 1;
    std::vector<PlantingCell> cells;

    for (int row = firstRow; row <= lastRow; ++row) {
        for (int column = firstColumn; column <= lastColumn; ++column) {
            cells.push_back(PlantingCell{column, row});
        }
    }

    return cells;
}

/*
* A crown's centre stands straight above its foot (placedCrownCentre), so every neighbour a cell's crowns may loop into
* has its foot within neighbourSpan of the cell: the cell grown by that, planted once. The trees keep plantBigTrees'
* order whatever box planted them, so each crown finds its neighbours in the same order (chooseLianas).
*/
NearTreeCell makeNearTreeCell(const PlantingCell& cell, float cellMetres, const NearTreeSources& sources, JobSystem& jobs) {
    const PlantingBox own = plantingCellBox(cell, cellMetres);
    const float margin = std::max(sources.lianas.neighbourSpan, 0.0f);
    const PlantingBox grown{.west = own.west - margin, .east = own.east + margin, .north = own.north - margin, .south = own.south + margin};
    NearTreeCell made;

    for (const NearTree& tree : plantBigTrees(sources.species, sources.maps, sources.heights, grown, jobs)) {
        const Vec3& foot = tree.placement.foot;

        if (sources.region && !insidePlantedRegion(sources.maps, *sources.region, foot)) {
            continue;
        }

        const ForestTreeSettings& settings = tree.layer == ForestLayer::Emergent ? sources.species.emergent : sources.species.canopy;
        const bool mine = foot.x >= own.west && foot.x < own.east && foot.z >= own.north && foot.z < own.south;
        made.trees.push_back(tree);
        made.crowns.push_back(LianaCrown{.centre = placedCrownCentre(tree.placement, settings),
                                         .radius = placedCrownRadius(tree.placement, settings),
                                         .identity = nearTreeIdentity(tree),
                                         .carries = mine});
    }

    made.lianas = chooseLianas(made.crowns, sources.lianas, sources.lianaSeed, forestHeight, forestPathDistance);
    return made;
}
