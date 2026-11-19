#include "scene/planting_maps.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace {
    constexpr float squareMetresAHectare = 10000.0f;
    constexpr float byteWhole = 255.0f;

    // The farthest the path map stores (65535 cm), read outside the maps.
    constexpr float farthestPath = 65535.0f;

    // Whether a place lies outside the maps' grid (across the axis, or past the first or last row).
    bool outsideTheMaps(const PlantingMaps& maps, float x, float z) {
        const float south = maps.northmost + static_cast<float>(maps.rows) * maps.texelMetres;
        return maps.rows <= 0 || maps.columns <= 0 || std::fabs(x - plantingAxisAt(maps, z)) > maps.halfWidth || z < maps.northmost || z > south;
    }

    // The four texels round a place on the grid and how far it is across and down between them.
    struct Blend {
        std::size_t northWest = 0;
        std::size_t northEast = 0;
        std::size_t southWest = 0;
        std::size_t southEast = 0;
        float across = 0.0f;
        float down = 0.0f;
    };

    Blend blendAt(const PlantingMaps& maps, float x, float z) {
        const Vec2 grid = plantingGridPlace(maps, x, z);
        const float column = std::clamp(grid.x - 0.5f, 0.0f, static_cast<float>(maps.columns - 1));
        const float row = std::clamp(grid.y - 0.5f, 0.0f, static_cast<float>(maps.rows - 1));
        const int west = static_cast<int>(column);
        const int north = static_cast<int>(row);
        const int east = std::min(west + 1, maps.columns - 1);
        const int south = std::min(north + 1, maps.rows - 1);
        const auto index = [&](int c, int r) {
            return static_cast<std::size_t>(r) * static_cast<std::size_t>(maps.columns) + static_cast<std::size_t>(c);
        };
        return Blend{index(west, north), index(east, north), index(west, south), index(east, south), column - static_cast<float>(west),
                     row - static_cast<float>(north)};
    }

    template <typename Read>
    float blended(const Blend& blend, Read read) {
        return terrainBilinear(read(blend.northWest), read(blend.northEast), read(blend.southWest), read(blend.southEast), blend.across, blend.down);
    }

    PlantingBox grown(const PlantingBox& box, float by) {
        return PlantingBox{box.west - by, box.east + by, box.north - by, box.south + by};
    }

    PlantingBox joined(const PlantingBox& a, const PlantingBox& b) {
        return PlantingBox{std::min(a.west, b.west), std::max(a.east, b.east), std::min(a.north, b.north), std::max(a.south, b.south)};
    }

    // A layer's lattice cells over a box, a cell more each way.
    struct LatticeRange {
        int firstColumn = 0;
        int firstRow = 0;
        int columns = 0;
        int rows = 0;

        bool holds(int column, int row) const {
            return column >= firstColumn && column < firstColumn + columns && row >= firstRow && row < firstRow + rows;
        }

        std::size_t index(int column, int row) const {
            return static_cast<std::size_t>(row - firstRow) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column - firstColumn);
        }
    };

    LatticeRange latticeOver(const PlantingBox& box, float footprint) {
        const int firstColumn = static_cast<int>(std::floor(box.west / footprint)) - 1;
        const int firstRow = static_cast<int>(std::floor(box.north / footprint)) - 1;
        const int lastColumn = static_cast<int>(std::floor(box.east / footprint)) + 1;
        const int lastRow = static_cast<int>(std::floor(box.south / footprint)) + 1;
        return LatticeRange{firstColumn, firstRow, lastColumn - firstColumn + 1, lastRow - firstRow + 1};
    }

    // A layer's plants standing over its range, by cell (-1 where none stands), for the layers after it.
    struct StandingLayer {
        LatticeRange range;
        std::vector<int> byCell;
        std::vector<PlantedPlant> plants;
    };

    // How far round a plant its own layer's neighbours can make it give way, in lattice cells.
    int spacingRings(const PlantingLayerRecord& layer) {
        return layer.spacingShare > 0.0f ? static_cast<int>(std::ceil(layer.spacingShare * plantingLayerReach(layer) / layer.footprint)) + 1 : 0;
    }

    float keepAwayReach(const PlantingKeepAway& keep, const std::vector<PlantingLayerRecord>& layers) {
        return keep.crown ? plantingLayerReach(layers[static_cast<std::size_t>(keep.layer)]) : keep.radius;
    }

    // Whether any plant of a layer before stands within a keep-away's reach of a place.
    bool keptAway(const PlantingKeepAway& keep, const StandingLayer& standing, float footprint, float reach, const Vec2& place) {
        const LatticeRange& range = standing.range;
        const int firstColumn = static_cast<int>(std::floor((place.x - reach) / footprint)) - 1;
        const int lastColumn = static_cast<int>(std::floor((place.x + reach) / footprint)) + 1;
        const int firstRow = static_cast<int>(std::floor((place.y - reach) / footprint)) - 1;
        const int lastRow = static_cast<int>(std::floor((place.y + reach) / footprint)) + 1;

        if (!range.holds(firstColumn, firstRow) || !range.holds(lastColumn, lastRow)) {
            throw std::logic_error("plantLayers: a layer read past what the layer before it planted");
        }

        for (int row = firstRow; row <= lastRow; ++row) {
            for (int column = firstColumn; column <= lastColumn; ++column) {
                const int other = standing.byCell[range.index(column, row)];

                if (other < 0) {
                    continue;
                }

                const PlantedPlant& plant = standing.plants[static_cast<std::size_t>(other)];

                if (plantingWithin(place, Vec2{plant.foot.x, plant.foot.z}, keep.crown ? plant.reach : keep.radius)) {
                    return true;
                }
            }
        }

        return false;
    }

    void checkLayers(const std::vector<PlantingLayerRecord>& layers) {
        for (std::size_t i = 0; i < layers.size(); ++i) {
            const PlantingLayerRecord& layer = layers[i];

            if (layer.under >= static_cast<int>(i) ||
                (layer.under >= 0 && layers[static_cast<std::size_t>(layer.under)].footprint != layer.footprint)) {
                throw std::invalid_argument("plantLayers: a layer shares the lattice of a later layer or one of another footprint");
            }

            for (const PlantingKeepAway& keep : layer.keepAway) {
                if (keep.layer < 0 || keep.layer >= static_cast<int>(i)) {
                    throw std::invalid_argument("plantLayers: a layer keeps away from itself or a later layer");
                }
            }

            const bool sized = (layer.reachPerScale.size() == 1 || static_cast<int>(layer.reachPerScale.size()) == layer.kinds) &&
                               layer.centrePerScale.size() == layer.reachPerScale.size();
            const bool shares = layer.kindShares.empty() ||
                                (static_cast<int>(layer.kindShares.size()) == layer.kinds - 1 && layer.kinds <= plantingMostSharedKinds);

            if (layer.kinds < 1 || layer.kinds > plantingMostKinds || !sized || !shares || !(layer.footprint > 0.0f)) {
                throw std::invalid_argument("plantLayers: a layer's kinds, sizes or footprint are out of range");
            }
        }
    }

    // A cell's plant before its own layer's spacing: none where the dither, the path, a layer before or the maps refuse.
    std::optional<PlantedPlant> candidateAt(const std::vector<PlantingLayerRecord>& layers, std::size_t which, const std::vector<StandingLayer>& standing,
                                            const PlantingMaps& maps, const TerrainHeights& heights, int column, int row) {
        const PlantingLayerRecord& layer = layers[which];
        const float footprint = layer.footprint;
        const float threshold = plantingThreshold(column, row);
        const float middleX = (static_cast<float>(column) + 0.5f) * footprint;
        const float middleZ = (static_cast<float>(row) + 0.5f) * footprint;
        const float below = layer.under >= 0 ? plantingLayerShare(layers[static_cast<std::size_t>(layer.under)], maps, middleX, middleZ) : 0.0f;

        if (!plantingTaken(threshold, below, plantingLayerShare(layer, maps, middleX, middleZ))) {
            return std::nullopt;
        }

        const std::uint64_t firstDraw = plantingCellKey(column, row) * plantingDrawsPerCell;
        const Vec2 place = plantingJitteredPlace(column, row, footprint, layer.jitterMetres, plantingRandom(layer.seed, firstDraw),
                                                 plantingRandom(layer.seed, firstDraw + 1));

        if (!plantingClearOfThePath(maps, place, layer.pathClearance)) {
            return std::nullopt;
        }

        for (const PlantingKeepAway& keep : layer.keepAway) {
            const PlantingLayerRecord& other = layers[static_cast<std::size_t>(keep.layer)];

            if (keptAway(keep, standing[static_cast<std::size_t>(keep.layer)], other.footprint, keepAwayReach(keep, layers), place)) {
                return std::nullopt;
            }
        }

        const std::uint64_t draws = firstDraw + plantingDrawsUsed;
        PlantedPlant plant;
        plant.scale = plantingScale(layer.leastScale, layer.scaleRange, plantingRandom(layer.seed, draws + PLANTING_DRAW_SCALE));
        plant.turnRadians = plantingTurn(plantingRandom(layer.seed, draws + PLANTING_DRAW_TURN));
        const float kindDraw = plantingRandom(layer.seed, draws + PLANTING_DRAW_KIND);

        if (layer.kindShares.empty()) {
            plant.kind = plantingUniformKind(kindDraw, layer.kinds);
        } else {
            std::array<float, plantingMostSharedKinds - 1> cumulative{};
            cumulative.fill(2.0f);
            std::copy(layer.kindShares.begin(), layer.kindShares.end(), cumulative.begin());
            plant.kind = plantingSharedKind(kindDraw, cumulative, layer.kinds);
        }

        plant.pick = plantingRandom(layer.seed, draws + PLANTING_DRAW_PICK);
        const std::size_t size = layer.reachPerScale.size() == 1 ? 0 : static_cast<std::size_t>(plant.kind);
        plant.reach = layer.reachPerScale[size] * plant.scale;

        float ground = terrainSurfaceHeight(heights, place.x, place.y);

        if (layer.footRingPerScale > 0.0f) {
            const float ring = layer.footRingPerScale * plant.scale;

            for (int k = 0; k < PLANTING_RING_POINTS; ++k) {
                const Vec2 point = plantingRingPoint(place, ring, k);
                ground = std::min(ground, terrainSurfaceHeight(heights, point.x, point.y));
            }
        }

        plant.foot = Vec3{place.x, ground - layer.footSink, place.y};

        if (!plantingFitsTheMaps(maps, place, plant.reach, plantingTop(plant.foot.y, layer.centrePerScale[size], plant.scale, plant.reach))) {
            return std::nullopt;
        }

        plant.threshold = threshold;
        plant.column = column;
        plant.row = row;
        return plant;
    }
}

float plantingAxisAt(const PlantingMaps& maps, float z) {
    if (maps.axis.empty()) {
        return 0.0f;
    }

    const float row = std::clamp(plantingCoordinate(z, maps.northmost, maps.inverseTexel) - 0.5f, 0.0f, static_cast<float>(maps.axis.size() - 1));
    const std::size_t north = static_cast<std::size_t>(row);
    const std::size_t south = std::min(north + 1, maps.axis.size() - 1);
    return plantingBetween(maps.axis[north], maps.axis[south], row - static_cast<float>(north));
}

Vec2 plantingGridPlace(const PlantingMaps& maps, float x, float z) {
    return Vec2{plantingCoordinate(x - plantingAxisAt(maps, z), -maps.halfWidth, maps.inverseTexel), plantingCoordinate(z, maps.northmost, maps.inverseTexel)};
}

std::array<float, 4> plantingEcotopes(const PlantingMaps& maps, float x, float z) {
    if (maps.ecotopes.empty() || outsideTheMaps(maps, x, z)) {
        return {0.0f, 0.0f, 0.0f, byteWhole};
    }

    const Blend blend = blendAt(maps, x, z);
    std::array<float, 4> weights{};

    for (std::size_t which = 0; which < weights.size(); ++which) {
        weights[which] = blended(blend, [&](std::size_t texel) { return static_cast<float>(maps.ecotopes[texel][which]); });
    }

    return weights;
}

float plantingGap(const PlantingMaps& maps, float x, float z) {
    if (maps.gaps.empty() || outsideTheMaps(maps, x, z)) {
        return 0.0f;
    }

    return blended(blendAt(maps, x, z), [&](std::size_t texel) { return static_cast<float>(maps.gaps[texel]); });
}

float plantingPathDistanceAt(const PlantingMaps& maps, float x, float z) {
    if (maps.path.empty() || outsideTheMaps(maps, x, z)) {
        return plantingPathDistance(farthestPath);
    }

    return plantingPathDistance(blended(blendAt(maps, x, z), [&](std::size_t texel) { return static_cast<float>(maps.path[texel]); }));
}

bool plantingClearOfThePath(const PlantingMaps& maps, const Vec2& place, float clearance) {
    for (std::size_t piece = 0; piece + 1 < maps.pathPoints.size(); ++piece) {
        const Vec2 start = maps.pathPoints[piece];
        const Vec2 along{maps.pathPoints[piece + 1].x - start.x, maps.pathPoints[piece + 1].y - start.y};

        if (plantingNearSegment(place, start, along, maps.pathInverseLengths[piece], clearance)) {
            return false;
        }
    }

    return true;
}

float plantingClearance(const PlantingMaps& maps, float x, float z) {
    const Vec2 grid = plantingGridPlace(maps, x, z);
    const int column = static_cast<int>(std::floor(grid.x));
    const int row = static_cast<int>(std::floor(grid.y));

    if (maps.clearance.empty() || column < 0 || column >= maps.columns || row < 0 || row >= maps.rows) {
        return plantingFarthestClearance;
    }

    const std::uint16_t stored = maps.clearance[static_cast<std::size_t>(row) * static_cast<std::size_t>(maps.columns) + static_cast<std::size_t>(column)];
    return plantingStored(static_cast<float>(stored), plantingDecimetre, maps.texelReach);
}

float plantingCap(const PlantingMaps& maps, float reach, float x, float z) {
    const auto cap = std::find_if(maps.caps.begin(), maps.caps.end(), [&](const PlantingCap& each) { return each.reach >= reach; });

    if (cap == maps.caps.end()) {
        throw std::invalid_argument("plantingCap: no canopy cap reaches that far");
    }

    const int column = static_cast<int>(std::floor(plantingCoordinate(x, cap->origin.x, maps.inverseTexel)));
    const int row = static_cast<int>(std::floor(plantingCoordinate(z, cap->origin.y, maps.inverseTexel)));

    if (column < 0 || column >= cap->columns || row < 0 || row >= cap->rows) {
        return plantingUncapped;
    }

    const std::uint16_t stored = cap->heights[static_cast<std::size_t>(row) * static_cast<std::size_t>(cap->columns) + static_cast<std::size_t>(column)];
    return stored == plantingNoCap ? plantingUncapped : plantingStored(static_cast<float>(stored), plantingDecimetre, plantingCapFloorMetres);
}

bool plantingFitsTheMaps(const PlantingMaps& maps, const Vec2& place, float reach, float top) {
    return plantingFits(plantingClearance(maps, place.x, place.y), reach, top, maps.corridorClearance, plantingCap(maps, reach, place.x, place.y));
}

void setPlantingDensity(PlantingLayerRecord& record, const PlantingDensity& density) {
    record.footprint = std::max(density.footprintMetres, 0.1f);
    const float cell = record.footprint * record.footprint / squareMetresAHectare;

    for (std::size_t which = 0; which < record.sharePerWeight.size(); ++which) {
        record.sharePerWeight[which] = density.perHectare[which] * cell / byteWhole;
    }

    record.gapStep = (density.gapShare - 1.0f) / byteWhole;
    record.nearPath = density.nearPathMetres;
    record.farPath = density.farPathMetres;
    record.farShare = density.farShare;
    record.inversePathSpan = density.farPathMetres > density.nearPathMetres ? 1.0f / (density.farPathMetres - density.nearPathMetres) : 0.0f;
    record.jitterMetres = density.jitterShare * record.footprint;
    record.pathClearance = density.pathClearance;
}

float plantingLayerReach(const PlantingLayerRecord& layer) {
    return *std::max_element(layer.reachPerScale.begin(), layer.reachPerScale.end()) * (layer.leastScale + layer.scaleRange);
}

float plantingLayerShare(const PlantingLayerRecord& layer, const PlantingMaps& maps, float x, float z) {
    const std::array<float, 4> weights = plantingEcotopes(maps, x, z);
    const float pathShare = plantingPathShare(plantingPathDistanceAt(maps, x, z), layer.nearPath, layer.farPath, layer.farShare, layer.inversePathSpan);
    return plantingShare({weights[0], weights[1], weights[2]}, layer.sharePerWeight, plantingGap(maps, x, z), layer.gapStep, pathShare);
}

/*
* Backwards first: each layer's box is the asked one joined with what every later layer reads of it (that layer's
* candidates' box grown by the keep-away's reach and a cell of each lattice either way), its candidates' box its own grown
* by its spacing's rings. Then forwards: each layer's candidates on the jobs (a row each), then those its spacing keeps,
* by cell for the layers after, and those inside the asked box for the caller.
*/
std::vector<std::vector<PlantedPlant>> plantLayers(const std::vector<PlantingLayerRecord>& layers, const PlantingMaps& maps,
                                                   const TerrainHeights& heights, const PlantingBox& box, JobSystem& jobs) {
    checkLayers(layers);
    const std::size_t count = layers.size();
    std::vector<PlantingBox> needed(count, box);
    std::vector<PlantingBox> candidateBoxes(count, box);

    for (std::size_t back = count; back-- > 0;) {
        const PlantingLayerRecord& layer = layers[back];
        candidateBoxes[back] = grown(needed[back], static_cast<float>(spacingRings(layer) + 1) * layer.footprint);

        for (const PlantingKeepAway& keep : layer.keepAway) {
            const std::size_t other = static_cast<std::size_t>(keep.layer);
            const float reach = keepAwayReach(keep, layers) + 2.0f * layer.footprint + 2.0f * layers[other].footprint;
            needed[other] = joined(needed[other], grown(candidateBoxes[back], reach));
        }
    }

    std::vector<StandingLayer> standing(count);
    std::vector<std::vector<PlantedPlant>> planted(count);

    for (std::size_t which = 0; which < count; ++which) {
        const PlantingLayerRecord& layer = layers[which];
        const LatticeRange candidateRange = latticeOver(candidateBoxes[which], layer.footprint);
        std::vector<std::optional<PlantedPlant>> candidates(static_cast<std::size_t>(candidateRange.columns) * static_cast<std::size_t>(candidateRange.rows));

        jobs.parallelFor(static_cast<std::size_t>(candidateRange.rows), 1, [&](std::size_t rowIndex) {
            const int row = candidateRange.firstRow + static_cast<int>(rowIndex);

            for (int column = candidateRange.firstColumn; column < candidateRange.firstColumn + candidateRange.columns; ++column) {
                candidates[candidateRange.index(column, row)] = candidateAt(layers, which, standing, maps, heights, column, row);
            }
        });

        StandingLayer& kept = standing[which];
        kept.range = latticeOver(needed[which], layer.footprint);
        kept.byCell.assign(static_cast<std::size_t>(kept.range.columns) * static_cast<std::size_t>(kept.range.rows), -1);
        const int rings = spacingRings(layer);

        for (int row = kept.range.firstRow; row < kept.range.firstRow + kept.range.rows; ++row) {
            for (int column = kept.range.firstColumn; column < kept.range.firstColumn + kept.range.columns; ++column) {
                const std::optional<PlantedPlant>& candidate = candidates[candidateRange.index(column, row)];

                if (!candidate) {
                    continue;
                }

                const Vec2 place{candidate->foot.x, candidate->foot.z};
                bool givesWay = false;

                for (int r = row - rings; r <= row + rings && !givesWay; ++r) {
                    for (int c = column - rings; c <= column + rings && !givesWay; ++c) {
                        const std::optional<PlantedPlant>& other = candidates[candidateRange.index(c, r)];

                        if ((c == column && r == row) || !other) {
                            continue;
                        }

                        const bool earlier = r < row || (r == row && c < column);
                        givesWay = plantingGivesWay(place, candidate->reach, candidate->threshold, Vec2{other->foot.x, other->foot.z}, other->reach,
                                                    other->threshold, earlier, layer.spacingShare);
                    }
                }

                if (givesWay) {
                    continue;
                }

                kept.byCell[kept.range.index(column, row)] = static_cast<int>(kept.plants.size());
                kept.plants.push_back(*candidate);

                if (place.x >= box.west && place.x < box.east && place.y >= box.north && place.y < box.south) {
                    planted[which].push_back(*candidate);
                }
            }
        }
    }

    return planted;
}
