#include "scene/planting_cells.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace {
    // The squared distance from a place to a cell's nearest point.
    float nearestSquared(const PlantingCell& cell, float cellMetres, const Vec2& place) {
        const PlantingBox box = plantingCellBox(cell, cellMetres);
        const float dx = std::max({box.west - place.x, 0.0f, place.x - box.east});
        const float dz = std::max({box.north - place.y, 0.0f, place.y - box.south});
        return dx * dx + dz * dz;
    }

    float dropDistance(const PlantingCellSettings& settings) {
        return settings.radiusMetres + settings.cellMetres;
    }

    void checkSettings(const PlantingCellSettings& settings) {
        if (!(settings.cellMetres > 0.0f) || !(settings.radiusMetres >= 0.0f)) {
            throw std::invalid_argument("Planting cells need a positive size and a radius of at least 0");
        }
    }
}

void visit(PlantingCellSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Enabled", settings.enabled);
    visitor.field("Cell", settings.cellMetres, {.min = 8.0f, .max = 1024.0f, .sliderMax = 256.0f, .unit = "m"});
    visitor.field("Radius", settings.radiusMetres, {.min = 0.0f, .max = 20000.0f, .sliderMax = 2000.0f, .unit = "m"});
    visitor.field("Cells a frame", settings.cellsAFrame, {.min = 0, .max = 256, .sliderMax = 32});
}

PlantingBox plantingCellBox(const PlantingCell& cell, float cellMetres) {
    const float west = static_cast<float>(cell.column) * cellMetres;
    const float north = static_cast<float>(cell.row) * cellMetres;
    return PlantingBox{west, west + cellMetres, north, north + cellMetres};
}

// Cells touching a span of twice the drop distance along each axis: at most ceil(span / cell) + 1 a side.
int plantingSlotCount(const PlantingCellSettings& settings) {
    checkSettings(settings);
    const int side = static_cast<int>(std::ceil(2.0f * dropDistance(settings) / settings.cellMetres)) + 1;
    return side * side;
}

/*
* Each layer's room in a slot: its lattice over any cell's box. Its candidates' room: its lattice over the candidates'
* box of a cell, every cell's the same size (plantingLayerBoxes grows each by the same amounts).
*/
PlantingSlotLayout plantingSlotLayout(const std::vector<PlantingLayerRecord>& layers, float cellMetres) {
    checkPlantingLayers(layers);
    const std::vector<PlantingLayerBoxes> boxes = plantingLayerBoxes(layers, plantingCellBox(PlantingCell{}, cellMetres));
    PlantingSlotLayout layout;

    for (std::size_t which = 0; which < layers.size(); ++which) {
        const float footprint = layers[which].footprint;
        const PlantingBox& candidates = boxes[which].candidates;
        const auto plants = static_cast<std::uint32_t>(plantingLatticeCapacity(cellMetres, cellMetres, footprint));
        const auto scratch =
            static_cast<std::uint32_t>(plantingLatticeCapacity(candidates.east - candidates.west, candidates.south - candidates.north, footprint));
        layout.layerFirst.push_back(layout.plantsASlot);
        layout.layerCapacity.push_back(plants);
        layout.plantsASlot += plants;
        layout.scratchFirst.push_back(layout.candidatesACell);
        layout.scratchCapacity.push_back(scratch);
        layout.candidatesACell += scratch;
    }

    return layout;
}

std::vector<PlantingCellMade> PlantingCellRing::update(const Vec2& camera, const PlantingCellSettings& settings) {
    checkSettings(settings);

    if (settings.cellMetres != cellMetres || settings.radiusMetres != radiusMetres) {
        reset();
        cellMetres = settings.cellMetres;
        radiusMetres = settings.radiusMetres;
        slotTaken.assign(static_cast<std::size_t>(plantingSlotCount(settings)), false);
    }

    const float drop = dropDistance(settings);

    for (auto cell = held.begin(); cell != held.end();) {
        if (nearestSquared(cell->first, cellMetres, camera) > drop * drop) {
            slotTaken[static_cast<std::size_t>(cell->second)] = false;
            cell = held.erase(cell);
        } else {
            ++cell;
        }
    }

    // The cells within the radius not yet held, nearest first, then by row and column.
    std::vector<std::tuple<float, int, int>> wanted;
    const float reach = radiusMetres;
    const int firstColumn = static_cast<int>(std::floor((camera.x - reach) / cellMetres)) - 1;
    const int lastColumn = static_cast<int>(std::floor((camera.x + reach) / cellMetres)) + 1;
    const int firstRow = static_cast<int>(std::floor((camera.y - reach) / cellMetres)) - 1;
    const int lastRow = static_cast<int>(std::floor((camera.y + reach) / cellMetres)) + 1;

    for (int row = firstRow; row <= lastRow; ++row) {
        for (int column = firstColumn; column <= lastColumn; ++column) {
            const PlantingCell cell{column, row};
            const float distance = nearestSquared(cell, cellMetres, camera);

            if (distance <= reach * reach && !held.contains(cell)) {
                wanted.emplace_back(distance, row, column);
            }
        }
    }

    std::sort(wanted.begin(), wanted.end());
    std::vector<PlantingCellMade> made;
    std::size_t freeSlot = 0;

    for (const auto& [distance, row, column] : wanted) {
        if (static_cast<int>(made.size()) >= settings.cellsAFrame) {
            break;
        }

        while (freeSlot < slotTaken.size() && slotTaken[freeSlot]) {
            ++freeSlot;
        }

        if (freeSlot == slotTaken.size()) {
            break;
        }

        slotTaken[freeSlot] = true;
        const PlantingCell cell{column, row};
        held.emplace(cell, static_cast<int>(freeSlot));
        made.push_back(PlantingCellMade{cell, static_cast<int>(freeSlot)});
    }

    return made;
}

void PlantingCellRing::reset() {
    held.clear();
    std::fill(slotTaken.begin(), slotTaken.end(), false);
    cellMetres = 0.0f;
    radiusMetres = 0.0f;
}

const std::map<PlantingCell, int>& PlantingCellRing::getHeld() const {
    return held;
}

int PlantingCellRing::getSlotCount() const {
    return static_cast<int>(slotTaken.size());
}
