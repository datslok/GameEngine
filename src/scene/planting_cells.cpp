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

    float dropDistance(const PlantingCellSetSettings& settings) {
        return settings.radiusMetres + settings.cellMetres;
    }

    void checkSettings(const PlantingCellSetSettings& settings) {
        if (!(settings.cellMetres > 0.0f) || !(settings.radiusMetres >= 0.0f)) {
            throw std::invalid_argument("Planting cells need a positive size and a radius of at least 0");
        }
    }
}

void visit(PlantingCellSetSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Cell", settings.cellMetres, {.min = 8.0f, .max = 1024.0f, .sliderMax = 256.0f, .unit = "m"});
    visitor.field("Radius", settings.radiusMetres, {.min = 0.0f, .max = 20000.0f, .sliderMax = 2000.0f, .unit = "m"});
    visitor.field("Cells a frame", settings.cellsAFrame, {.min = 0, .max = 256, .sliderMax = 32});
}

void visit(PlantingCellSettings& settings, SettingsVisitor& visitor) {
    static constexpr std::array<const char*, plantingCellSetCount> names{"Coarse cells", "Middle cells", "Fine cells"};
    visitor.field("Enabled", settings.enabled);

    for (int set = 0; set < plantingCellSetCount; ++set) {
        visitor.group(names[static_cast<std::size_t>(set)], [&] {
            visit(settings.sets[static_cast<std::size_t>(set)], visitor);
        });
    }
}

/*
* Backwards, so a layer read is marked before its own turn comes (layers read only earlier ones): each needed layer
* marks what it reads. Then forwards, the needed ones renumbered and their references with them.
*/
PlantingSetLayers plantingSetLayers(const std::vector<PlantingLayerRecord>& layers, int set) {
    std::vector<bool> needed(layers.size(), false);

    for (std::size_t back = layers.size(); back-- > 0;) {
        const PlantingLayerRecord& layer = layers[back];

        if (layer.cellSet < 0 || layer.cellSet >= plantingCellSetCount) {
            throw std::invalid_argument("plantingSetLayers: a layer names no set of cells");
        }

        if (layer.cellSet == set) {
            needed[back] = true;
        }

        if (!needed[back]) {
            continue;
        }

        if (layer.under >= 0 && static_cast<std::size_t>(layer.under) < back) {
            needed[static_cast<std::size_t>(layer.under)] = true;
        }

        for (const PlantingKeepAway& keep : layer.keepAway) {
            if (keep.layer >= 0 && static_cast<std::size_t>(keep.layer) < back) {
                needed[static_cast<std::size_t>(keep.layer)] = true;
            }
        }
    }

    PlantingSetLayers made;
    std::vector<int> renumbered(layers.size(), -1);

    for (std::size_t which = 0; which < layers.size(); ++which) {
        if (!needed[which]) {
            continue;
        }

        renumbered[which] = static_cast<int>(made.layers.size());
        PlantingLayerRecord layer = layers[which];

        if (layer.under >= 0) {
            layer.under = renumbered[static_cast<std::size_t>(layer.under)];
        }

        for (PlantingKeepAway& keep : layer.keepAway) {
            keep.layer = renumbered[static_cast<std::size_t>(keep.layer)];
        }

        made.written.push_back(layer.cellSet == set);
        made.layers.push_back(std::move(layer));
        made.fieldLayer.push_back(static_cast<int>(which));
    }

    return made;
}

PlantingBox plantingCellBox(const PlantingCell& cell, float cellMetres) {
    const float west = static_cast<float>(cell.column) * cellMetres;
    const float north = static_cast<float>(cell.row) * cellMetres;
    return PlantingBox{west, west + cellMetres, north, north + cellMetres};
}

// Cells touching a span of twice the drop distance along each axis: at most ceil(span / cell) + 1 a side.
int plantingSlotCount(const PlantingCellSetSettings& settings) {
    checkSettings(settings);
    const int side = static_cast<int>(std::ceil(2.0f * dropDistance(settings) / settings.cellMetres)) + 1;
    return side * side;
}

/*
* Each written layer's room in a slot: its lattice over any cell's box. Its candidates' room: its lattice over the
* candidates' box of a cell, every cell's the same size (plantingLayerBoxes grows each by the same amounts).
*/
PlantingSlotLayout plantingSlotLayout(const std::vector<PlantingLayerRecord>& layers, float cellMetres, const std::vector<bool>& written) {
    checkPlantingLayers(layers);

    if (!written.empty() && written.size() != layers.size()) {
        throw std::invalid_argument("plantingSlotLayout: a written flag a layer");
    }

    const std::vector<PlantingLayerBoxes> boxes = plantingLayerBoxes(layers, plantingCellBox(PlantingCell{}, cellMetres));
    PlantingSlotLayout layout;
    layout.written = written.empty() ? std::vector<bool>(layers.size(), true) : written;

    for (std::size_t which = 0; which < layers.size(); ++which) {
        const float footprint = layers[which].footprint;
        const PlantingBox& candidates = boxes[which].candidates;
        const auto plants =
            layout.written[which] ? static_cast<std::uint32_t>(plantingLatticeCapacity(cellMetres, cellMetres, footprint)) : std::uint32_t{0};
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

std::vector<PlantingCellMade> PlantingCellRing::update(const Vec2& camera, const PlantingCellSetSettings& settings) {
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
