#include "games/forest/forest_planting.h"

#include <algorithm>
#include <iterator>

namespace {
    constexpr int layerOf(ForestLayer layer) {
        return static_cast<int>(layer);
    }

    std::vector<TreePlacement> trees(const std::vector<PlantedPlant>& plants) {
        std::vector<TreePlacement> placed;
        placed.reserve(plants.size());

        for (const PlantedPlant& plant : plants) {
            placed.push_back(TreePlacement{plant.foot, plant.scale, plant.turnRadians, plant.kind});
        }

        return placed;
    }

    // Each kept place's plants only.
    template <typename Placement>
    std::vector<Placement> inside(const std::vector<Placement>& placed, const PlantingRegion& region) {
        std::vector<Placement> kept;
        std::copy_if(placed.begin(), placed.end(), std::back_inserter(kept), [&](const Placement& each) { return insideRegion(region, each.foot); });
        return kept;
    }
}

std::vector<PlantingLayerRecord> forestPlantingLayers(const ForestSpecies& species) {
    std::vector<PlantingLayerRecord> layers(forestLayerCount);
    const auto at = [&](ForestLayer layer) -> PlantingLayerRecord& {
        return layers[static_cast<std::size_t>(layerOf(layer))];
    };

    at(ForestLayer::Emergent) = treePlantingLayer(species.emergent, emergentPlaceSeed);

    PlantingLayerRecord& canopy = at(ForestLayer::Canopy);
    canopy = treePlantingLayer(species.canopy, canopyPlaceSeed);
    canopy.under = canopy.footprint == at(ForestLayer::Emergent).footprint ? layerOf(ForestLayer::Emergent) : -1;
    canopy.keepAway = {PlantingKeepAway{.layer = layerOf(ForestLayer::Emergent), .crown = true}};

    PlantingLayerRecord& mid = at(ForestLayer::Mid);
    mid = treePlantingLayer(species.mid, midPlaceSeed);
    mid.keepAway = {PlantingKeepAway{.layer = layerOf(ForestLayer::Emergent), .radius = midTrunkClearance},
                    PlantingKeepAway{.layer = layerOf(ForestLayer::Canopy), .radius = midTrunkClearance}};

    const auto clearOfTrunks = [&](float clearance) {
        return std::vector<PlantingKeepAway>{PlantingKeepAway{.layer = layerOf(ForestLayer::Emergent), .radius = bigTrunkReach + clearance},
                                             PlantingKeepAway{.layer = layerOf(ForestLayer::Canopy), .radius = bigTrunkReach + clearance},
                                             PlantingKeepAway{.layer = layerOf(ForestLayer::Mid), .radius = midTrunkReach + clearance}};
    };

    at(ForestLayer::Understorey) = understoreyPlantingLayer(species.understorey);
    at(ForestLayer::Understorey).keepAway = clearOfTrunks(species.understorey.trunkClearance);
    at(ForestLayer::Palms) = palmPlantingLayer(species.palms);
    at(ForestLayer::Palms).keepAway = clearOfTrunks(species.palms.trunkClearance);
    return layers;
}

ForestPlanting plantTheForest(const ForestSpecies& species, const ValleyMaps& maps, const TerrainHeights& heights, const PlantingBox& box,
                              JobSystem& jobs) {
    const std::vector<std::vector<PlantedPlant>> planted = plantLayers(forestPlantingLayers(species), maps.planting, heights, box, jobs);
    const auto layer = [&](ForestLayer which) -> const std::vector<PlantedPlant>& {
        return planted[static_cast<std::size_t>(layerOf(which))];
    };

    ForestPlanting forest;
    forest.emergents = trees(layer(ForestLayer::Emergent));
    forest.canopy = trees(layer(ForestLayer::Canopy));
    forest.mid = trees(layer(ForestLayer::Mid));

    for (const PlantedPlant& plant : layer(ForestLayer::Understorey)) {
        forest.understorey.push_back(UnderstoreyPlacement{plant.foot, plant.scale, plant.turnRadians, static_cast<UnderstoreyKind>(plant.kind),
                                                          std::min(plant.pick, 0.99999f)});
    }

    for (const PlantedPlant& plant : layer(ForestLayer::Palms)) {
        forest.palms.push_back(PalmPlacement{plant.foot, plant.scale, plant.turnRadians, plant.kind});
    }

    return forest;
}

ForestPlanting plantThePatch(const ForestSpecies& species, const ValleyMaps& maps, const TerrainHeights& heights, JobSystem& jobs) {
    const ForestPlanting all = plantTheForest(species, maps, heights, regionBox(forestPatch), jobs);
    return ForestPlanting{inside(all.emergents, forestPatch), inside(all.canopy, forestPatch), inside(all.mid, forestPatch),
                          inside(all.understorey, forestPatch), inside(all.palms, forestPatch)};
}
