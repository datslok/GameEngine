#include "games/forest/forest_planting.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <utility>

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
    std::vector<Placement> inside(const std::vector<Placement>& placed, const PlantingMaps& maps, const PlantedRegion& region) {
        std::vector<Placement> kept;
        std::copy_if(placed.begin(), placed.end(), std::back_inserter(kept), [&](const Placement& each) { return insidePlantedRegion(maps, region, each.foot); });
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
    // The mid trees far outnumber the tall ones (480 a hectare against about 33), so they stand only as far as the
    // understorey; past it the sparse tall trees carry the far forest (Battlefield 3's payloads, VALLEY.md).
    mid.cellSet = plantingMiddleCells;
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
    at(ForestLayer::Understorey).cellSet = plantingMiddleCells;
    at(ForestLayer::Palms).cellSet = plantingMiddleCells;
    return layers;
}

PlantingLayerRecord forestCoverLayer(float cellMetres, int variants, int steps, int coverMap, std::uint64_t seed) {
    const int kinds = std::max(variants, 1) * std::max(steps, 1);

    if (kinds > plantingMostKinds) {
        throw std::invalid_argument("forestCoverLayer: more patch kinds than a layer holds");
    }

    PlantingLayerRecord layer;
    layer.footprint = cellMetres;
    layer.seed = seed;
    layer.kinds = kinds;
    layer.reachPerScale = {cellMetres * static_cast<float>(PLANTING_RING_DIAGONAL)};
    layer.centrePerScale = {0.0f};
    layer.coverMap = coverMap;
    layer.cellSet = plantingFineCells;
    return layer;
}

int forestCoverKind(const GroundCell& cell, int steps) {
    return cell.variant * std::max(steps, 1) + cell.step - 1;
}

PlantingCoverMap forestCoverMap(const std::vector<GroundCell>& cells, float cellMetres, int steps) {
    PlantingCoverMap map;

    if (cells.empty()) {
        return map;
    }

    const auto latticeOf = [&](const GroundCell& cell) {
        return std::pair<int, int>{static_cast<int>(std::floor(cell.centre.x / cellMetres)), static_cast<int>(std::floor(cell.centre.y / cellMetres))};
    };
    int firstColumn = latticeOf(cells.front()).first;
    int lastColumn = firstColumn;
    int firstRow = latticeOf(cells.front()).second;
    int lastRow = firstRow;

    for (const GroundCell& cell : cells) {
        const auto [column, row] = latticeOf(cell);
        firstColumn = std::min(firstColumn, column);
        lastColumn = std::max(lastColumn, column);
        firstRow = std::min(firstRow, row);
        lastRow = std::max(lastRow, row);
    }

    map.firstColumn = firstColumn;
    map.firstRow = firstRow;
    map.columns = lastColumn - firstColumn + 1;
    map.rows = lastRow - firstRow + 1;
    map.values.assign(static_cast<std::size_t>(map.columns) * static_cast<std::size_t>(map.rows), 0);

    for (const GroundCell& cell : cells) {
        const auto [column, row] = latticeOf(cell);
        const std::size_t at = static_cast<std::size_t>(row - firstRow) * static_cast<std::size_t>(map.columns) + static_cast<std::size_t>(column - firstColumn);
        map.values[at] = static_cast<std::uint8_t>(plantingCoverValue(forestCoverKind(cell, steps), cell.quarterTurns));
    }

    return map;
}

float forestCoverRelief(const std::vector<GroundCell>& cells, float cellMetres, const TerrainHeights& heights, JobSystem& jobs) {
    // How far past its square a patch's blades and leaves may lean (the tallest grass leans under half a metre).
    constexpr float leanMargin = 1.0f;
    const float half = 0.5f * cellMetres + leanMargin;
    std::vector<float> reliefs(cells.size(), 0.0f);
    jobs.parallelFor(cells.size(), 64, [&](std::size_t index) {
        const Vec2& centre = cells[index].centre;
        const float foot = terrainSurfaceHeight(heights, centre.x, centre.y);
        const TerrainHeightRange range = terrainHeightRange(heights, centre.x - half, centre.x + half, centre.y - half, centre.y + half);
        reliefs[index] = std::max(range.highest - foot, foot - range.lowest);
    });
    return reliefs.empty() ? 0.0f : *std::max_element(reliefs.begin(), reliefs.end());
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
    const PlantingMaps& planting = maps.planting;
    return ForestPlanting{inside(all.emergents, planting, forestPatch), inside(all.canopy, planting, forestPatch), inside(all.mid, planting, forestPatch),
                          inside(all.understorey, planting, forestPatch), inside(all.palms, planting, forestPatch)};
}
