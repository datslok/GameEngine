#pragma once

#include "scene/planting_maps.h"
#include "scene/terrain_heights.h"

#include "test_jobs.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Maps, ground and layers for the planting tests (planting_test, planting_cells_test, planting_records_test).

// Maps over a straight axis (x = 0) from z = -200, 2 m texels: floor and rim weights varying across, a gap band and
// the path along x = 10, the clearance far from everything but a disc of obstacle at (-40, -100), no caps but one
// high above everything.
inline PlantingMaps straightMaps() {
    PlantingMaps maps;
    maps.columns = 150;
    maps.rows = 150;
    maps.texelMetres = 2.0f;
    maps.inverseTexel = 0.5f;
    maps.halfWidth = 150.0f;
    maps.northmost = -200.0f;
    maps.texelReach = 0.5f * std::sqrt(2.0f) * maps.texelMetres;
    maps.corridorClearance = 2.0f;
    maps.axis.assign(static_cast<std::size_t>(maps.rows), 0.0f);
    const std::size_t texels = static_cast<std::size_t>(maps.columns) * static_cast<std::size_t>(maps.rows);
    maps.ecotopes.resize(texels);
    maps.gaps.resize(texels);
    maps.path.resize(texels);
    maps.clearance.resize(texels);

    for (int row = 0; row < maps.rows; ++row) {
        for (int column = 0; column < maps.columns; ++column) {
            const float x = -maps.halfWidth + (static_cast<float>(column) + 0.5f) * maps.texelMetres;
            const float z = maps.northmost + (static_cast<float>(row) + 0.5f) * maps.texelMetres;
            const std::size_t texel = static_cast<std::size_t>(row) * static_cast<std::size_t>(maps.columns) + static_cast<std::size_t>(column);
            const auto rim = static_cast<std::uint8_t>(std::clamp(std::fabs(x) * 1.5f, 0.0f, 255.0f));
            maps.ecotopes[texel] = {static_cast<std::uint8_t>(255 - rim), 0, rim, 0};
            maps.gaps[texel] = static_cast<std::uint8_t>(std::fabs(z + 100.0f) < 20.0f ? 255 : 0);
            maps.path[texel] = static_cast<std::uint16_t>(std::fabs(x - 10.0f) * 100.0f);
            const float obstacle = std::max(std::sqrt((x + 40.0f) * (x + 40.0f) + (z + 100.0f) * (z + 100.0f)) - 8.0f, 0.0f);
            maps.clearance[texel] = static_cast<std::uint16_t>(std::floor(std::min(obstacle, plantingFarthestClearance) * 10.0f));
        }
    }

    maps.pathPoints = {Vec2{10.0f, -1000.0f}, Vec2{10.0f, 1000.0f}};
    maps.pathInverseLengths = {1.0f / (2000.0f * 2000.0f)};
    maps.caps.push_back(PlantingCap{.reach = 40.0f, .origin = Vec2{-150.0f, -200.0f}, .columns = 1, .rows = 1, .heights = {plantingNoCap}});
    return maps;
}

// Rolling ground on a 2 m grid round the maps.
inline const TerrainHeights& rollingGround() {
    static const TerrainHeights heights = [] {
        TerrainHeights made;
        made.grids.push_back(makeTerrainHeightGrid(Vec2{0.0f, -50.0f}, 200.0f, 2.0f, [](float x, float z) {
            return 3.0f * std::sin(0.05f * x) + 2.0f * std::cos(0.07f * z);
        }, testJobs()));
        return made;
    }();
    return heights;
}

inline PlantingLayerRecord layerOf(const PlantingDensity& density, std::uint64_t seed, float reach) {
    PlantingLayerRecord layer;
    setPlantingDensity(layer, density);
    layer.seed = seed;
    layer.reachPerScale = {reach};
    layer.centrePerScale = {reach};
    layer.leastScale = 0.85f;
    layer.scaleRange = 0.3f;
    return layer;
}

/*
* Three layers as the forest's: big trees spaced by their crowns on a 12 m lattice, small trees on another clear of
* their trunks, ground plants of three kinds by shares clear of every trunk, standing on the ground's lowest round
* the trunk (the big ones) or sunk (the plants).
*/
inline std::vector<PlantingLayerRecord> threeLayers() {
    PlantingLayerRecord big = layerOf(PlantingDensity{.perHectare = {60.0f, 0.0f, 40.0f}, .footprintMetres = 12.0f, .jitterShare = 0.8f,
                                                      .gapShare = 0.3f, .pathClearance = 4.0f},
                                      0x1234u, 8.0f);
    big.kinds = 4;
    big.centrePerScale = {20.0f};
    big.footRingPerScale = 2.5f;
    big.spacingShare = 0.8f;

    PlantingLayerRecord small = layerOf(PlantingDensity{.perHectare = {500.0f, 0.0f, 300.0f}, .footprintMetres = 4.0f, .jitterShare = 0.9f,
                                                        .gapShare = 1.5f, .pathClearance = 2.0f},
                                        0x5678u, 3.0f);
    small.spacingShare = 0.5f;
    small.keepAway = {PlantingKeepAway{.layer = 0, .radius = 4.0f}};

    PlantingLayerRecord plants = layerOf(PlantingDensity{.perHectare = {3000.0f, 0.0f, 1000.0f}, .footprintMetres = 1.5f, .jitterShare = 0.8f,
                                                         .gapShare = 2.0f, .pathClearance = 1.5f, .nearPathMetres = 20.0f, .farPathMetres = 60.0f,
                                                         .farShare = 0.35f},
                                         0x9abcu, 0.7f);
    plants.kinds = 3;
    plants.kindShares = {0.45f, 0.75f};
    plants.reachPerScale = {0.6f, 0.7f, 0.8f};
    plants.centrePerScale = plants.reachPerScale;
    plants.footSink = 0.05f;
    plants.keepAway = {PlantingKeepAway{.layer = 0, .radius = 3.0f}, PlantingKeepAway{.layer = 1, .radius = 1.4f}};
    return {big, small, plants};
}

inline bool samePlant(const PlantedPlant& a, const PlantedPlant& b) {
    return a.foot.x == b.foot.x && a.foot.y == b.foot.y && a.foot.z == b.foot.z && a.scale == b.scale && a.turnRadians == b.turnRadians &&
           a.kind == b.kind && a.pick == b.pick && a.reach == b.reach && a.threshold == b.threshold && a.column == b.column && a.row == b.row;
}
