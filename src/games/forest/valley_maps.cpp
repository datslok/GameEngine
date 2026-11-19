#include "games/forest/valley_maps.h"

#include "games/forest/forest_terrain.h"
#include "math/smooth_step.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
    constexpr float squareMetresAHectare = 10000.0f;
    constexpr float decimetresAMetre = 10.0f;
    constexpr float uncapped = 1e9f;
    constexpr float capRounding = 0.05f; // taken off a cap, so float rounding cannot lift it over a line
    constexpr std::uint32_t gapSeed = 0x47415053u;
    constexpr int gapOctaves = 3;

    // Roberts' R2 sequence's steps (1 / g and 1 / g^2 for the plastic number g) in 32-bit fixed point.
    constexpr std::uint32_t ditherStepColumn = 3242174889u;
    constexpr std::uint32_t ditherStepRow = 2447445414u;

    // The extent's grid: how many texels span a length.
    int texelsOver(float metres, float texel) {
        return std::max(static_cast<int>(std::ceil(metres / texel - 1e-4f)), 1);
    }

    std::uint16_t toDecimetres(float metres, float offset) {
        const float decimetres = std::floor((metres + offset) * decimetresAMetre);
        return static_cast<std::uint16_t>(std::clamp(decimetres, 0.0f, static_cast<float>(noCap - 1)));
    }

    // A texel's middle in the world, on the grid across the valley's axis.
    Vec2 texelMiddle(const ForestPlantingSettings& settings, int column, int row) {
        const float z = settings.northmost + (static_cast<float>(row) + 0.5f) * settings.texelMetres;
        const float across = -settings.halfWidth + (static_cast<float>(column) + 0.5f) * settings.texelMetres;
        return Vec2{valleyAxis(z) + across, z};
    }

    /*
    * The farthest a place lies from its texel's middle across the ground: half a texel each way on the grid, but the
    * axis's slope between their z shifts x by up to slope times half a texel more.
    */
    float texelReachOver(const ForestPlantingSettings& settings, int rows) {
        const float texel = settings.texelMetres;
        float steepest = 0.0f;

        for (int row = 0; row <= rows; ++row) {
            const float z = settings.northmost + static_cast<float>(row) * texel;
            steepest = std::max(steepest, std::fabs(valleyAxis(z + 0.5f * texel) - valleyAxis(z - 0.5f * texel)) / texel);
        }

        const float slope = 1.01f * steepest + 1e-4f;
        return 0.5f * texel * std::sqrt((1.0f + slope) * (1.0f + slope) + 1.0f);
    }

    // Cumulative rounding: each weight to 0..255 with the four summing to exactly 255.
    std::array<std::uint8_t, ecotopeCount> quantised(const EcotopeWeights& weights) {
        std::array<std::uint8_t, ecotopeCount> bytes{};
        float sum = 0.0f;
        int before = 0;

        for (int index = 0; index < ecotopeCount; ++index) {
            sum += weights[static_cast<std::size_t>(index)];
            const int upTo = index + 1 == ecotopeCount ? 255 : std::clamp(static_cast<int>(std::lround(sum * 255.0f)), before, 255);
            bytes[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(upTo - before);
            before = upTo;
        }

        return bytes;
    }

    // The ecotopes: each texel's height first, then its slope from its neighbours' (the axis's slope turns the grid's).
    void buildEcotopes(ValleyMaps& maps, JobSystem& jobs) {
        const ForestPlantingSettings& settings = maps.settings;
        const int columns = maps.columns;
        const int rows = maps.rows;
        std::vector<float> heights(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows));

        jobs.parallelFor(static_cast<std::size_t>(rows), 1, [&](std::size_t row) {
            for (int column = 0; column < columns; ++column) {
                const Vec2 middle = texelMiddle(settings, column, static_cast<int>(row));
                heights[row * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column)] = forestHeight(middle.x, middle.y);
            }
        });

        maps.ecotopes.resize(heights.size());
        const float texel = settings.texelMetres;

        jobs.parallelFor(static_cast<std::size_t>(rows), 1, [&](std::size_t rowIndex) {
            const int row = static_cast<int>(rowIndex);
            const int north = std::max(row - 1, 0);
            const int south = std::min(row + 1, rows - 1);
            const float z = settings.northmost + (static_cast<float>(row) + 0.5f) * texel;
            const float axisSlope = (valleyAxis(z + 0.5f * texel) - valleyAxis(z - 0.5f * texel)) / texel;

            for (int column = 0; column < columns; ++column) {
                const int west = std::max(column - 1, 0);
                const int east = std::min(column + 1, columns - 1);
                const auto height = [&](int c, int r) {
                    return heights[static_cast<std::size_t>(r) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(c)];
                };
                const float alongX = (height(east, row) - height(west, row)) / (static_cast<float>(std::max(east - west, 1)) * texel);
                const float alongGridZ = (height(column, south) - height(column, north)) / (static_cast<float>(std::max(south - north, 1)) * texel);
                const float alongZ = alongGridZ - alongX * axisSlope;
                const Vec2 middle = texelMiddle(settings, column, row);
                const float slope = std::sqrt(alongX * alongX + alongZ * alongZ);
                maps.ecotopes[rowIndex * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column)] =
                    quantised(landEcotopes(settings, middle.x, middle.y, slope));
            }
        });
    }

    // The clearance: each texel's distance to the nearest obstacle's edge, every row against the obstacles near it.
    void buildClearance(ValleyMaps& maps, const std::vector<ValleyObstacle>& obstacles, JobSystem& jobs) {
        const ForestPlantingSettings& settings = maps.settings;
        const int columns = maps.columns;
        maps.clearance.assign(static_cast<std::size_t>(columns) * static_cast<std::size_t>(maps.rows), toDecimetres(farthestClearance, 0.0f));

        jobs.parallelFor(static_cast<std::size_t>(maps.rows), 1, [&](std::size_t row) {
            const float z = settings.northmost + (static_cast<float>(row) + 0.5f) * settings.texelMetres;

            for (const ValleyObstacle& obstacle : obstacles) {
                const float reach = obstacle.radius + farthestClearance;

                if (std::fabs(z - obstacle.centre.y) > reach) {
                    continue;
                }

                const float across = obstacle.centre.x - valleyAxis(z) + settings.halfWidth;
                const int first = std::max(static_cast<int>(std::floor((across - reach) / settings.texelMetres)), 0);
                const int last = std::min(static_cast<int>(std::floor((across + reach) / settings.texelMetres)), columns - 1);

                for (int column = first; column <= last; ++column) {
                    const Vec2 middle = texelMiddle(settings, column, static_cast<int>(row));
                    const float distance = std::max((middle - obstacle.centre).length() - obstacle.radius, 0.0f);
                    std::uint16_t& texel = maps.clearance[row * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column)];
                    texel = std::min(texel, toDecimetres(std::min(distance, farthestClearance), 0.0f));
                }
            }
        });
    }

    // The lowest a line passes over the ground within reach of a place (across the ground), or uncapped if it does not.
    float lowestWithin(const SightLine& line, const Vec2& place, float reach) {
        const Vec2 start{line.from.x, line.from.z};
        const Vec2 along = Vec2{line.to.x, line.to.z} - start;
        const Vec2 off = start - place;
        const float lengthSquared = along.lengthSquared();

        if (lengthSquared < 1e-8f) {
            return off.length() <= reach ? std::min(line.from.y, line.to.y) : uncapped;
        }

        // Where |off + s along| = reach: the share s of the line within reach is between the quadratic's roots.
        const float half = along.dot(off);
        const float discriminant = half * half - lengthSquared * (off.lengthSquared() - reach * reach);

        if (discriminant < 0.0f) {
            return uncapped;
        }

        const float root = std::sqrt(discriminant);
        const float first = std::max((-half - root) / lengthSquared, 0.0f);
        const float last = std::min((-half + root) / lengthSquared, 1.0f);

        if (first > last) {
            return uncapped;
        }

        const float rise = line.to.y - line.from.y;
        return line.from.y + rise * (rise >= 0.0f ? first : last);
    }

    /*
    * The canopy caps, over the lines' footprint grown by the largest reach: at each texel's middle, for each reach, the
    * lowest any line passes within that reach and half a texel's diagonal more (so the cap holds anywhere in the texel),
    * from where each line enters and leaves that disc (a line is straight, so its lowest point there is at one end).
    * Each row tests only the lines passing near it.
    */
    void buildCaps(ValleyMaps& maps, const std::vector<SightLine>& lines, std::vector<float> reaches, JobSystem& jobs) {
        std::sort(reaches.begin(), reaches.end());
        reaches.erase(std::unique(reaches.begin(), reaches.end()), reaches.end());

        if (lines.empty() || reaches.empty()) {
            for (const float reach : reaches) {
                maps.caps.push_back(CanopyCap{.reach = reach, .origin = Vec2{0.0f, 0.0f}, .columns = 0, .rows = 0, .heights = {}});
            }

            return;
        }

        const float texel = maps.settings.texelMetres;
        const float texelHalfDiagonal = 0.5f * std::sqrt(2.0f) * texel;
        const float margin = reaches.back() + texelHalfDiagonal + texel;
        Vec2 least{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        Vec2 most{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};

        for (const SightLine& line : lines) {
            for (const Vec3& end : {line.from, line.to}) {
                least = Vec2{std::min(least.x, end.x), std::min(least.y, end.z)};
                most = Vec2{std::max(most.x, end.x), std::max(most.y, end.z)};
            }
        }

        const Vec2 origin{std::floor((least.x - margin) / texel) * texel, std::floor((least.y - margin) / texel) * texel};
        const int columns = texelsOver(most.x + margin - origin.x, texel);
        const int rows = texelsOver(most.y + margin - origin.y, texel);
        const std::size_t texels = static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows);

        for (const float reach : reaches) {
            maps.caps.push_back(CanopyCap{.reach = reach, .origin = origin, .columns = columns, .rows = rows, .heights = std::vector<std::uint16_t>(texels, noCap)});
        }

        jobs.parallelFor(static_cast<std::size_t>(rows), 1, [&](std::size_t row) {
            const float z = origin.y + (static_cast<float>(row) + 0.5f) * texel;
            std::vector<const SightLine*> near;

            for (const SightLine& line : lines) {
                if (z >= std::min(line.from.z, line.to.z) - margin && z <= std::max(line.from.z, line.to.z) + margin) {
                    near.push_back(&line);
                }
            }

            for (int column = 0; column < columns; ++column) {
                const Vec2 middle{origin.x + (static_cast<float>(column) + 0.5f) * texel, z};
                const std::size_t index = row * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column);

                for (const SightLine* line : near) {
                    if (middle.x < std::min(line->from.x, line->to.x) - margin || middle.x > std::max(line->from.x, line->to.x) + margin) {
                        continue;
                    }

                    for (CanopyCap& cap : maps.caps) {
                        const float lowest = lowestWithin(*line, middle, cap.reach + texelHalfDiagonal);

                        if (lowest < uncapped) {
                            cap.heights[index] = std::min(cap.heights[index], toDecimetres(lowest - capRounding, capFloorMetres));
                        }
                    }
                }
            }
        });
    }

    // The texel a place is in on the grid across the axis, or false outside the extent.
    bool texelOf(const ValleyMaps& maps, float x, float z, int& column, int& row) {
        const ForestPlantingSettings& settings = maps.settings;
        column = static_cast<int>(std::floor((x - valleyAxis(z) + settings.halfWidth) / settings.texelMetres));
        row = static_cast<int>(std::floor((z - settings.northmost) / settings.texelMetres));
        return column >= 0 && column < maps.columns && row >= 0 && row < maps.rows;
    }

    // A cell of a lattice as one number, for its random numbers.
    std::uint64_t cellKey(int column, int row) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(column)) << 32) | static_cast<std::uint32_t>(row);
    }
}

void visit(ForestPlantingSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Texel", settings.texelMetres, {.min = 0.5f, .max = 16.0f, .sliderMax = 8.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Half width", settings.halfWidth, {.min = 10.0f, .max = 2000.0f, .sliderMax = 800.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Northmost", settings.northmost, {.min = -20000.0f, .max = 1000.0f, .sliderMin = -8000.0f, .sliderMax = 0.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Southmost", settings.southmost, {.min = -20000.0f, .max = 2000.0f, .sliderMin = -1000.0f, .sliderMax = 1000.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Ecotope blend", settings.ecotopeBlendMetres, {.min = 0.5f, .max = 200.0f, .sliderMax = 80.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Bare slope from", settings.bareSlopeFrom, {.min = 0.0f, .max = 10.0f, .sliderMax = 4.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Bare slope to", settings.bareSlopeTo, {.min = 0.01f, .max = 10.0f, .sliderMax = 4.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Clearing edge", settings.clearingEdgeMetres, {.min = 0.5f, .max = 100.0f, .sliderMax = 40.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Gap size", settings.gapMetres, {.min = 1.0f, .max = 1000.0f, .sliderMax = 200.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Gap from", settings.gapFrom, {.min = -1.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Gap to", settings.gapTo, {.min = -1.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Corridor clearance", settings.corridorClearance, {.min = 0.0f, .max = 50.0f, .sliderMax = 10.0f, .unit = "m", .applies = FieldApplies::AtStartup});
}

void visit(PlantingLayer& layer, SettingsVisitor& visitor) {
    visitor.field("Floor a hectare", layer.floorPerHectare, {.min = 0.0f, .max = 20000.0f, .sliderMax = 5000.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Wall a hectare", layer.wallPerHectare, {.min = 0.0f, .max = 20000.0f, .sliderMax = 5000.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Rim a hectare", layer.rimPerHectare, {.min = 0.0f, .max = 20000.0f, .sliderMax = 5000.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Footprint", layer.footprintMetres, {.min = 0.5f, .max = 100.0f, .sliderMax = 30.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Jitter", layer.jitterShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Gap share", layer.gapShare, {.min = 0.0f, .max = 10.0f, .sliderMax = 4.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Path clearance", layer.pathClearance, {.min = 0.0f, .max = 50.0f, .sliderMax = 15.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Near path", layer.nearPathMetres, {.min = 0.0f, .max = 500.0f, .sliderMax = 150.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Far path", layer.farPathMetres, {.min = 0.0f, .max = 500.0f, .sliderMax = 150.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Far share", layer.farShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
}

std::vector<ValleyObstacle> forestObstacles(const std::vector<PillarPlacement>& pillars, const std::vector<BastionShape>& towers) {
    std::vector<ValleyObstacle> obstacles;
    obstacles.reserve(pillars.size() + 2 + towers.size());

    for (const PillarPlacement& pillar : pillars) {
        obstacles.push_back(ValleyObstacle{Vec2{pillar.foot.x, pillar.foot.z}, pillarBulgeShare * pillar.radius});
    }

    for (const float side : {-1.0f, 1.0f}) {
        obstacles.push_back(ValleyObstacle{Vec2{forestRingPlace.x + side * forestRingRadius, forestRingPlace.y}, ringFootShare * forestRingTubeRadius});
    }

    for (const BastionShape& tower : towers) {
        float widest = tower.glacisWidth;

        for (const BastionTier& tier : tower.tiers) {
            widest = std::max(widest, tier.width);
        }

        const float halfDiagonal = 0.5f * widest * std::sqrt(1.0f + tower.depthShare * tower.depthShare);
        obstacles.push_back(ValleyObstacle{Vec2{tower.foot.x, tower.foot.z}, halfDiagonal});
    }

    return obstacles;
}

EcotopeWeights landEcotopes(const ForestPlantingSettings& settings, float x, float z, float slope) {
    const float across = std::fabs(x - valleyAxis(z));
    EcotopeWeights weights{};

    if (across > settings.halfWidth || z < settings.northmost || z > settings.southmost) {
        weights[static_cast<std::size_t>(Ecotope::Bare)] = 1.0f;
        return weights;
    }

    const float blend = settings.ecotopeBlendMetres;
    const float wall = smoothStep(valleyFloorHalfWidth - blend, valleyFloorHalfWidth + blend, across);
    const float rimEdge = valleyFloorHalfWidth + valleyWallWidth;
    const float rim = smoothStep(rimEdge - blend, rimEdge + blend, across);
    const float edge = settings.clearingEdgeMetres;
    const float clearing = 1.0f - smoothStep(forestClearingRadius, forestClearingRadius + edge, (Vec2{x, z} - forestClearingCentre).length());
    const float meadow = 1.0f - smoothStep(forestMeadowRadius, forestMeadowRadius + edge, (Vec2{x, z} - forestStartPlace).length());
    const float steep = settings.bareSlopeTo > settings.bareSlopeFrom ? smoothStep(settings.bareSlopeFrom, settings.bareSlopeTo, slope)
                                                                      : (slope >= settings.bareSlopeTo ? 1.0f : 0.0f);
    const float bare = std::max({clearing, meadow, steep});
    const float kept = 1.0f - bare;
    weights[static_cast<std::size_t>(Ecotope::Floor)] = kept * (1.0f - wall);
    weights[static_cast<std::size_t>(Ecotope::Wall)] = kept * wall * (1.0f - rim);
    weights[static_cast<std::size_t>(Ecotope::Rim)] = kept * wall * rim;
    weights[static_cast<std::size_t>(Ecotope::Bare)] = bare;
    return weights;
}

ValleyMaps buildValleyMaps(const ForestPlantingSettings& settings, const std::vector<ValleyObstacle>& obstacles,
                           const std::vector<SightLine>& lines, const std::vector<float>& capReaches, JobSystem& jobs) {
    ValleyMaps maps;
    maps.settings = settings;
    maps.settings.texelMetres = std::max(settings.texelMetres, 0.1f);
    maps.columns = texelsOver(2.0f * settings.halfWidth, maps.settings.texelMetres);
    maps.rows = texelsOver(settings.southmost - settings.northmost, maps.settings.texelMetres);
    maps.texelReach = texelReachOver(maps.settings, maps.rows);
    buildEcotopes(maps, jobs);
    buildClearance(maps, obstacles, jobs);
    buildCaps(maps, lines, capReaches, jobs);
    return maps;
}

EcotopeWeights valleyEcotopes(const ValleyMaps& maps, float x, float z) {
    const ForestPlantingSettings& settings = maps.settings;
    const float across = x - valleyAxis(z);
    EcotopeWeights weights{};

    if (std::fabs(across) > settings.halfWidth || z < settings.northmost || z > settings.southmost || maps.ecotopes.empty()) {
        weights[static_cast<std::size_t>(Ecotope::Bare)] = 1.0f;
        return weights;
    }

    const float gridColumn = (across + settings.halfWidth) / settings.texelMetres - 0.5f;
    const float gridRow = (z - settings.northmost) / settings.texelMetres - 0.5f;
    const float column = std::clamp(gridColumn, 0.0f, static_cast<float>(maps.columns - 1));
    const float row = std::clamp(gridRow, 0.0f, static_cast<float>(maps.rows - 1));
    const int west = static_cast<int>(column);
    const int north = static_cast<int>(row);
    const int east = std::min(west + 1, maps.columns - 1);
    const int south = std::min(north + 1, maps.rows - 1);
    const float eastShare = column - static_cast<float>(west);
    const float southShare = row - static_cast<float>(north);
    const auto texel = [&](int c, int r) {
        return maps.ecotopes[static_cast<std::size_t>(r) * static_cast<std::size_t>(maps.columns) + static_cast<std::size_t>(c)];
    };

    for (std::size_t which = 0; which < weights.size(); ++which) {
        const float northern = static_cast<float>(texel(west, north)[which]) * (1.0f - eastShare) + static_cast<float>(texel(east, north)[which]) * eastShare;
        const float southern = static_cast<float>(texel(west, south)[which]) * (1.0f - eastShare) + static_cast<float>(texel(east, south)[which]) * eastShare;
        weights[which] = (northern * (1.0f - southShare) + southern * southShare) / 255.0f;
    }

    return weights;
}

float valleyClearance(const ValleyMaps& maps, float x, float z) {
    int column = 0;
    int row = 0;

    if (!texelOf(maps, x, z, column, row)) {
        return farthestClearance;
    }

    const std::uint16_t stored = maps.clearance[static_cast<std::size_t>(row) * static_cast<std::size_t>(maps.columns) + static_cast<std::size_t>(column)];
    return static_cast<float>(stored) / decimetresAMetre - maps.texelReach;
}

float valleyCap(const ValleyMaps& maps, float reach, float x, float z) {
    const auto cap = std::find_if(maps.caps.begin(), maps.caps.end(), [&](const CanopyCap& each) { return each.reach >= reach; });

    if (cap == maps.caps.end()) {
        throw std::invalid_argument("valleyCap: no canopy cap reaches that far");
    }

    const float texel = maps.settings.texelMetres;
    const int column = static_cast<int>(std::floor((x - cap->origin.x) / texel));
    const int row = static_cast<int>(std::floor((z - cap->origin.y) / texel));

    if (column < 0 || column >= cap->columns || row < 0 || row >= cap->rows) {
        return uncapped;
    }

    const std::uint16_t stored = cap->heights[static_cast<std::size_t>(row) * static_cast<std::size_t>(cap->columns) + static_cast<std::size_t>(column)];
    return stored == noCap ? uncapped : static_cast<float>(stored) / decimetresAMetre - capFloorMetres;
}

float valleyGap(const ForestPlantingSettings& settings, float x, float z) {
    const float noise = forestFractalNoise(x / settings.gapMetres, z / settings.gapMetres, gapOctaves, gapSeed);

    if (settings.gapTo <= settings.gapFrom) {
        return noise >= settings.gapTo ? 1.0f : 0.0f;
    }

    return smoothStep(settings.gapFrom, settings.gapTo, noise);
}

float pathDensityShare(const PlantingLayer& layer, float pathDistance) {
    if (pathDistance <= layer.nearPathMetres) {
        return 1.0f;
    }

    if (pathDistance >= layer.farPathMetres) {
        return layer.farShare;
    }

    const float share = (pathDistance - layer.nearPathMetres) / (layer.farPathMetres - layer.nearPathMetres);
    return 1.0f + (layer.farShare - 1.0f) * share;
}

float layerShare(const PlantingLayer& layer, const ValleyMaps& maps, float x, float z) {
    const EcotopeWeights weights = valleyEcotopes(maps, x, z);
    const float perHectare = weights[static_cast<std::size_t>(Ecotope::Floor)] * layer.floorPerHectare +
                             weights[static_cast<std::size_t>(Ecotope::Wall)] * layer.wallPerHectare +
                             weights[static_cast<std::size_t>(Ecotope::Rim)] * layer.rimPerHectare;

    if (perHectare <= 0.0f) {
        return 0.0f;
    }

    const float cell = layer.footprintMetres * layer.footprintMetres / squareMetresAHectare;
    const float gap = 1.0f + (layer.gapShare - 1.0f) * valleyGap(maps.settings, x, z);
    return std::clamp(perHectare * cell * gap * pathDensityShare(layer, forestPathDistance(x, z)), 0.0f, 1.0f);
}

float ditherThreshold(int column, int row) {
    const std::uint32_t bits = static_cast<std::uint32_t>(column) * ditherStepColumn + static_cast<std::uint32_t>(row) * ditherStepRow;
    return static_cast<float>(bits >> 8) * (1.0f / 16777216.0f);
}

std::vector<DitheredCell> ditheredCells(const PlantingLayer& layer, const ValleyMaps& maps, const PlantingRegion& region, std::uint64_t seed,
                                        const PlantingLayer* under) {
    if (under != nullptr && under->footprintMetres != layer.footprintMetres) {
        throw std::invalid_argument("ditheredCells: a layer sharing a lattice needs the same footprint");
    }

    std::vector<DitheredCell> cells;
    const float footprint = std::max(layer.footprintMetres, 0.1f);
    const int firstRow = static_cast<int>(std::floor(region.northmost / footprint));
    const int lastRow = static_cast<int>(std::floor(region.southmost / footprint));

    for (int row = firstRow; row <= lastRow; ++row) {
        const float north = static_cast<float>(row) * footprint;
        const float axisWest = std::min({valleyAxis(north), valleyAxis(north + 0.5f * footprint), valleyAxis(north + footprint)});
        const float axisEast = std::max({valleyAxis(north), valleyAxis(north + 0.5f * footprint), valleyAxis(north + footprint)});
        const int firstColumn = static_cast<int>(std::floor((axisWest - region.spread) / footprint)) - 1;
        const int lastColumn = static_cast<int>(std::floor((axisEast + region.spread) / footprint)) + 1;

        for (int column = firstColumn; column <= lastColumn; ++column) {
            const Vec2 middle{(static_cast<float>(column) + 0.5f) * footprint, (static_cast<float>(row) + 0.5f) * footprint};
            const float threshold = ditherThreshold(column, row);
            const float below = under != nullptr ? layerShare(*under, maps, middle.x, middle.y) : 0.0f;

            if (threshold < below || threshold >= below + std::min(layerShare(layer, maps, middle.x, middle.y), 1.0f - below)) {
                continue;
            }

            const std::uint64_t firstDraw = cellKey(column, row) * ditherDrawsPerCell;
            const Vec2 place = middle + Vec2{forestUnitRandom(seed, firstDraw) - 0.5f, forestUnitRandom(seed, firstDraw + 1) - 0.5f} * (layer.jitterShare * footprint);

            if (place.y < region.northmost || place.y > region.southmost || std::fabs(place.x - valleyAxis(place.y)) > region.spread ||
                forestPathDistance(place.x, place.y) < layer.pathClearance) {
                continue;
            }

            cells.push_back(DitheredCell{.column = column, .row = row, .place = place, .threshold = threshold, .firstDraw = firstDraw + ditherDrawsUsed});
        }
    }

    return cells;
}

bool fitsTheMaps(const ValleyMaps& maps, const Vec2& place, float reach, float topHeight) {
    return valleyClearance(maps, place.x, place.y) >= reach &&
           topHeight + maps.settings.corridorClearance <= valleyCap(maps, reach, place.x, place.y);
}
