#include "scene/planting_maps.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace {
    constexpr float squareMetresAHectare = 10000.0f;
    constexpr float byteWhole = 255.0f;

    // The farthest the path map stores (65535 cm), read outside the maps.
    constexpr float farthestPath = static_cast<float>(PLANTING_FARTHEST_PATH);

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

    // A layer's plants standing over its range, by cell (-1 where none stands), for the layers after it.
    struct StandingLayer {
        PlantingLattice range;
        std::vector<int> byCell;
        std::vector<PlantedPlant> plants;
    };

    // Whether any plant of a layer before stands within a keep-away's reach of a place.
    bool keptAway(const PlantingKeepAway& keep, const StandingLayer& standing, float footprint, float reach, const Vec2& place) {
        const PlantingLattice& range = standing.range;
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

    // A cover layer's cell's plant: the one its map names, at the cell's middle on the ground (PlantingLayerRecord).
    std::optional<PlantedPlant> coverAt(const PlantingLayerRecord& layer, const PlantingMaps& maps, const TerrainHeights& heights, int column, int row) {
        const int value = plantingCoverAt(maps.coverMaps[static_cast<std::size_t>(layer.coverMap)], column, row);

        if (value == 0) {
            return std::nullopt;
        }

        const std::uint64_t draws = plantingCellKey(column, row) * plantingDrawsPerCell + plantingDrawsUsed;
        const Vec2 place = plantingJitteredPlace(column, row, layer.footprint, 0.0f, 0.5f, 0.5f);
        PlantedPlant plant;
        plant.scale = plantingScale(layer.leastScale, layer.scaleRange, plantingRandom(layer.seed, draws + PLANTING_DRAW_SCALE));
        plant.turnRadians = plantingCoverTurn(value);
        plant.kind = plantingCoverKind(value);
        plant.pick = plantingRandom(layer.seed, draws + PLANTING_DRAW_PICK);
        const std::size_t size = layer.reachPerScale.size() == 1 ? 0 : static_cast<std::size_t>(plant.kind);
        plant.reach = layer.reachPerScale[size] * plant.scale;
        plant.foot = Vec3{place.x, terrainSurfaceHeight(heights, place.x, place.y) - layer.footSink, place.y};
        plant.threshold = plantingThreshold(column, row);
        plant.column = column;
        plant.row = row;
        return plant;
    }

    // A cell's plant before its own layer's spacing: none where the dither, the path, a layer before or the maps refuse.
    std::optional<PlantedPlant> candidateAt(const std::vector<PlantingLayerRecord>& layers, std::size_t which, const std::vector<StandingLayer>& standing,
                                            const PlantingMaps& maps, const TerrainHeights& heights, int column, int row) {
        const PlantingLayerRecord& layer = layers[which];

        if (layer.coverMap >= 0) {
            return coverAt(layer, maps, heights, column, row);
        }

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

            if (keptAway(keep, standing[static_cast<std::size_t>(keep.layer)], other.footprint, plantingKeepAwayReach(keep, layers), place)) {
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

bool PlantingLattice::holds(int column, int row) const {
    return column >= firstColumn && column < firstColumn + columns && row >= firstRow && row < firstRow + rows;
}

std::size_t PlantingLattice::index(int column, int row) const {
    return static_cast<std::size_t>(row - firstRow) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column - firstColumn);
}

std::size_t PlantingLattice::count() const {
    return static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows);
}

PlantingLattice plantingLatticeOver(const PlantingBox& box, float footprint) {
    const int firstColumn = static_cast<int>(std::floor(box.west / footprint)) - 1;
    const int firstRow = static_cast<int>(std::floor(box.north / footprint)) - 1;
    const int lastColumn = static_cast<int>(std::floor(box.east / footprint)) + 1;
    const int lastRow = static_cast<int>(std::floor(box.south / footprint)) + 1;
    return PlantingLattice{firstColumn, firstRow, lastColumn - firstColumn + 1, lastRow - firstRow + 1};
}

// floor(a + w) - floor(a) is at most ceil(w); the cell either way makes three more, and one more covers the division's rounding.
std::size_t plantingLatticeCapacity(float width, float height, float footprint) {
    const auto side = [footprint](float extent) {
        return static_cast<std::size_t>(std::ceil(std::max(extent, 0.0f) / footprint)) + 4;
    };
    return side(width) * side(height);
}

int plantingSpacingRings(const PlantingLayerRecord& layer) {
    return layer.spacingShare > 0.0f ? static_cast<int>(std::ceil(layer.spacingShare * plantingLayerReach(layer) / layer.footprint)) + 1 : 0;
}

float plantingKeepAwayReach(const PlantingKeepAway& keep, const std::vector<PlantingLayerRecord>& layers) {
    return keep.crown ? plantingLayerReach(layers[static_cast<std::size_t>(keep.layer)]) : keep.radius;
}

/*
* Backwards: each layer's standing box is the asked one joined with what every later layer reads of it (that layer's
* candidates' box grown by the keep-away's reach and two cells of each lattice), its candidates' box its own grown by its
* spacing's rings and a cell.
*/
std::vector<PlantingLayerBoxes> plantingLayerBoxes(const std::vector<PlantingLayerRecord>& layers, const PlantingBox& box) {
    std::vector<PlantingLayerBoxes> boxes(layers.size(), PlantingLayerBoxes{box, box});

    for (std::size_t back = layers.size(); back-- > 0;) {
        const PlantingLayerRecord& layer = layers[back];
        boxes[back].candidates = grown(boxes[back].standing, static_cast<float>(plantingSpacingRings(layer) + 1) * layer.footprint);

        for (const PlantingKeepAway& keep : layer.keepAway) {
            const std::size_t other = static_cast<std::size_t>(keep.layer);
            const float reach = plantingKeepAwayReach(keep, layers) + 2.0f * layer.footprint + 2.0f * layers[other].footprint;
            boxes[other].standing = joined(boxes[other].standing, grown(boxes[back].candidates, reach));
        }
    }

    return boxes;
}

void checkPlantingLayers(const std::vector<PlantingLayerRecord>& layers) {
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

        if (layer.coverMap >= 0 && (layer.under >= 0 || !layer.keepAway.empty())) {
            throw std::invalid_argument("plantLayers: a cover layer shares a lattice or keeps away from another layer");
        }
    }
}

void checkPlantingCover(const std::vector<PlantingLayerRecord>& layers, const PlantingMaps& maps) {
    for (const PlantingLayerRecord& layer : layers) {
        if (layer.coverMap < 0) {
            continue;
        }

        if (layer.coverMap >= static_cast<int>(maps.coverMaps.size())) {
            throw std::invalid_argument("plantLayers: a cover layer names a map the maps lack");
        }

        const PlantingCoverMap& map = maps.coverMaps[static_cast<std::size_t>(layer.coverMap)];

        if (map.columns < 0 || map.rows < 0 || map.values.size() != static_cast<std::size_t>(map.columns) * static_cast<std::size_t>(map.rows)) {
            throw std::invalid_argument("plantLayers: a cover map's size is not its columns times its rows");
        }

        for (const std::uint8_t value : map.values) {
            if (value != 0 && plantingCoverKind(value) >= layer.kinds) {
                throw std::invalid_argument("plantLayers: a cover map names a kind its layer lacks");
            }
        }
    }
}

int plantingCoverAt(const PlantingCoverMap& map, int column, int row) {
    const int across = column - map.firstColumn;
    const int down = row - map.firstRow;

    if (across < 0 || down < 0 || across >= map.columns || down >= map.rows) {
        return 0;
    }

    return map.values[static_cast<std::size_t>(down) * static_cast<std::size_t>(map.columns) + static_cast<std::size_t>(across)];
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

// The axis from the maps rather than the game's own curve, so the GPU (which has only the maps) agrees at the edges.
bool insidePlantedRegion(const PlantingMaps& maps, const PlantedRegion& region, const Vec3& foot) {
    return foot.z >= region.northmost && foot.z <= region.southmost && std::fabs(foot.x - plantingAxisAt(maps, foot.z)) <= region.spread;
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
* Each layer's boxes first (plantingLayerBoxes), then forwards: each layer's candidates on the jobs (a row each), then
* those its spacing keeps, by cell for the layers after, and those inside the asked box for the caller.
*/
std::vector<std::vector<PlantedPlant>> plantLayers(const std::vector<PlantingLayerRecord>& layers, const PlantingMaps& maps,
                                                   const TerrainHeights& heights, const PlantingBox& box, JobSystem& jobs) {
    checkPlantingLayers(layers);
    checkPlantingCover(layers, maps);
    const std::size_t count = layers.size();
    const std::vector<PlantingLayerBoxes> boxes = plantingLayerBoxes(layers, box);
    std::vector<StandingLayer> standing(count);
    std::vector<std::vector<PlantedPlant>> planted(count);

    for (std::size_t which = 0; which < count; ++which) {
        const PlantingLayerRecord& layer = layers[which];
        const PlantingLattice candidateRange = plantingLatticeOver(boxes[which].candidates, layer.footprint);
        std::vector<std::optional<PlantedPlant>> candidates(candidateRange.count());

        jobs.parallelFor(static_cast<std::size_t>(candidateRange.rows), 1, [&](std::size_t rowIndex) {
            const int row = candidateRange.firstRow + static_cast<int>(rowIndex);

            for (int column = candidateRange.firstColumn; column < candidateRange.firstColumn + candidateRange.columns; ++column) {
                candidates[candidateRange.index(column, row)] = candidateAt(layers, which, standing, maps, heights, column, row);
            }
        });

        StandingLayer& kept = standing[which];
        kept.range = plantingLatticeOver(boxes[which].standing, layer.footprint);
        kept.byCell.assign(kept.range.count(), -1);
        const int rings = plantingSpacingRings(layer);

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
