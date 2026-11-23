#include "render/gpu/liana_trees.h"

#include "render/gpu/bone_poses.h"

#include <cmath>
#include <stdexcept>

namespace {
    // The column (or row) whose span, from float(index) * size to that plus size, holds a place: plantingCellBox's edges.
    int indexOf(float place, float size) {
        int index = static_cast<int>(std::floor(place / size));
        const float west = static_cast<float>(index) * size;

        if (place < west) {
            --index;
        } else if (place >= west + size) {
            ++index;
        }

        return index;
    }

    // An index modulo side, 0 to side - 1 below 0 too (as lianaWrapped in liana_trees.glsl, whose % is undefined there).
    int wrapped(int index, int side) {
        const int remainder = index - (index / side) * side;
        return remainder < 0 ? remainder + side : remainder;
    }
}

LianaTreeTables packLianaTreeTables(std::span<const PlantingSetSlots> sets, std::size_t fieldLayers) {
    LianaTreeTables tables;
    tables.layers.assign(fieldLayers, GpuLianaTreeLayer{});

    for (const PlantingSetSlots& set : sets) {
        if (set.side == 0) {
            throw std::invalid_argument("packLianaTreeTables: a set of cells needs its side");
        }

        const auto firstCell = static_cast<std::uint32_t>(tables.cells.size());
        tables.cells.resize(tables.cells.size() + std::size_t{set.side} * set.side);

        for (const auto& [cell, slot] : set.held) {
            GpuLianaCell& place = tables.cells[firstCell + lianaCellPlace(cell, set.side)];

            if (place.slot != lianaCellEmpty) {
                throw std::logic_error("packLianaTreeTables: two held cells at one place of a set's table");
            }

            place = GpuLianaCell{.column = cell.column, .row = cell.row, .slot = static_cast<std::uint32_t>(slot)};
        }

        const auto layers = static_cast<std::uint32_t>(set.fieldLayer.size());

        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            if (!set.layout.written[layer]) {
                continue;
            }

            const auto field = static_cast<std::size_t>(set.fieldLayer[layer]);

            if (field >= fieldLayers) {
                throw std::invalid_argument("packLianaTreeTables: a set's layer past the field's");
            }

            tables.layers[field] = GpuLianaTreeLayer{.firstPlant = set.places.plantBase + set.layout.layerFirst[layer],
                                                     .plantsASlot = set.layout.plantsASlot,
                                                     .firstCount = set.places.countBase + layer,
                                                     .countsASlot = layers,
                                                     .cellMetres = set.cellMetres,
                                                     .firstCell = firstCell,
                                                     .side = set.side};
        }
    }

    return tables;
}

PlantingCell lianaTreeCell(float x, float z, float cellMetres) {
    return PlantingCell{indexOf(x, cellMetres), indexOf(z, cellMetres)};
}

std::uint32_t lianaCellPlace(const PlantingCell& cell, std::uint32_t side) {
    const int across = static_cast<int>(side);
    return static_cast<std::uint32_t>(wrapped(cell.column, across) + across * wrapped(cell.row, across));
}

bool lianaSameTree(const Vec3& planted, const Vec3& foot) {
    return (planted - foot).lengthSquared() < lianaTreeMatchMetres * lianaTreeMatchMetres;
}

std::uint32_t lianaTreePlant(const LianaShape& shape, int end, const LianaTreeTables& tables, std::span<const GpuPlacedPlant> plants,
                             std::span<const std::uint32_t> counts, const Vec3& eye, float fadeEnd) {
    const std::uint32_t layer = shape.anchors[2 + end];

    if (layer >= tables.layers.size()) {
        return BONE_NO_TREE;
    }

    const GpuLianaTreeLayer& record = tables.layers[layer];
    const float* tree = end == 0 ? shape.firstTree : shape.secondTree;
    const Vec3 foot{tree[0], tree[1], tree[2]};

    if (!(record.cellMetres > 0.0f) || !boneTreeWanted(foot, eye, fadeEnd)) {
        return BONE_NO_TREE;
    }

    const PlantingCell cell = lianaTreeCell(foot.x, foot.z, record.cellMetres);
    const std::size_t place = std::size_t{record.firstCell} + lianaCellPlace(cell, record.side);

    if (place >= tables.cells.size()) {
        return BONE_NO_TREE;
    }

    const GpuLianaCell& held = tables.cells[place];

    if (held.slot == lianaCellEmpty || held.column != cell.column || held.row != cell.row) {
        return BONE_NO_TREE;
    }

    const std::size_t countPlace = std::size_t{record.firstCount} + std::size_t{held.slot} * record.countsASlot;
    const std::size_t first = std::size_t{record.firstPlant} + std::size_t{held.slot} * record.plantsASlot;
    const std::uint32_t count = countPlace < counts.size() ? counts[countPlace] : 0u;

    for (std::size_t plant = first; plant < first + count && plant < plants.size(); ++plant) {
        if (lianaSameTree(Vec3{plants[plant].foot[0], plants[plant].foot[1], plants[plant].foot[2]}, foot)) {
            return static_cast<std::uint32_t>(plant);
        }
    }

    return BONE_NO_TREE;
}
