#include "scene/terrain_heights.h"

#include "scene/planting.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

TerrainHeightGrid makeTerrainHeightGrid(const Vec2& centre, float halfSize, float spacing, const std::function<float(float, float)>& height,
                                        JobSystem& jobs) {
    if (!(spacing > 0.0f) || !(halfSize > 0.0f)) {
        throw std::invalid_argument("A terrain grid needs a size and a spacing above 0");
    }

    TerrainHeightGrid grid;
    grid.corner = Vec2{centre.x - halfSize, centre.y - halfSize};
    grid.spacing = spacing;
    grid.size = static_cast<std::uint32_t>(std::lround(2.0f * halfSize / spacing)) + 1;
    grid.heights.resize(static_cast<std::size_t>(grid.size) * grid.size);
    jobs.parallelFor(grid.size, 1, [&](std::size_t row) {
        const float z = grid.corner.y + static_cast<float>(row) * spacing;

        for (std::uint32_t column = 0; column < grid.size; ++column) {
            grid.heights[row * grid.size + column] = height(grid.corner.x + static_cast<float>(column) * spacing, z);
        }
    });
    grid.pyramid = terrainHeightPyramid(grid, jobs);
    return grid;
}

int terrainPyramidLevels(std::uint32_t size) {
    int levels = 0;

    while (terrainPyramidLevelSize(size, levels) > 1u) {
        ++levels;
    }

    return levels;
}

// ceil(size / 2^level): each texel holds 2^level points a side, the last what is left.
std::uint32_t terrainPyramidLevelSize(std::uint32_t size, int level) {
    return (size + (1u << level) - 1u) >> level;
}

std::size_t terrainPyramidLevelStart(std::uint32_t size, int level) {
    std::size_t start = 0;

    for (int below = 1; below < level; ++below) {
        const std::size_t across = terrainPyramidLevelSize(size, below);
        start += across * across;
    }

    return start;
}

TerrainHeightRange terrainPyramidTexel(const TerrainHeightGrid& grid, int level, std::uint32_t column, std::uint32_t row) {
    if (level == 0) {
        const float height = grid.heights[static_cast<std::size_t>(row) * grid.size + column];
        return TerrainHeightRange{height, height};
    }

    const std::size_t pair = terrainPyramidLevelStart(grid.size, level) + static_cast<std::size_t>(row) * terrainPyramidLevelSize(grid.size, level) + column;
    return TerrainHeightRange{grid.pyramid[2 * pair], grid.pyramid[2 * pair + 1]};
}

// Each level from the one below (level 1 from the points): a texel the extremes of its 2 x 2, clamped at an odd side.
std::vector<float> terrainHeightPyramid(const TerrainHeightGrid& grid, JobSystem& jobs) {
    const int levels = terrainPyramidLevels(grid.size);
    TerrainHeightGrid built = grid;
    built.pyramid.assign(2 * terrainPyramidLevelStart(grid.size, levels + 1), 0.0f);

    for (int level = 1; level <= levels; ++level) {
        const std::uint32_t across = terrainPyramidLevelSize(grid.size, level);
        const std::uint32_t below = terrainPyramidLevelSize(grid.size, level - 1);
        const std::size_t start = terrainPyramidLevelStart(grid.size, level);
        jobs.parallelFor(across, 1, [&](std::size_t row) {
            const std::uint32_t top = 2 * static_cast<std::uint32_t>(row);

            for (std::uint32_t column = 0; column < across; ++column) {
                TerrainHeightRange range{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};

                for (std::uint32_t z = top; z < std::min(top + 2, below); ++z) {
                    for (std::uint32_t x = 2 * column; x < std::min(2 * column + 2, below); ++x) {
                        const TerrainHeightRange child = terrainPyramidTexel(built, level - 1, x, z);
                        range.lowest = std::min(range.lowest, child.lowest);
                        range.highest = std::max(range.highest, child.highest);
                    }
                }

                const std::size_t pair = start + row * across + column;
                built.pyramid[2 * pair] = range.lowest;
                built.pyramid[2 * pair + 1] = range.highest;
            }
        });
    }

    return built.pyramid;
}

float terrainBilinear(float nearLeft, float nearRight, float farLeft, float farRight, float across, float down) {
    const float upper = nearLeft + (nearRight - nearLeft) * across;
    const float lower = farLeft + (farRight - farLeft) * across;
    return upper + (lower - upper) * down;
}

namespace {
    // Whether a grid holds a place, its edges included.
    bool holds(const TerrainHeightGrid& grid, float x, float z) {
        const float extent = static_cast<float>(grid.size - 1) * grid.spacing;
        return x >= grid.corner.x && x <= grid.corner.x + extent && z >= grid.corner.y && z <= grid.corner.y + extent;
    }

    /*
    * The four points round a place, clamped onto the grid, interpolated by between (terrainBilinear or
    * terrainTriangleHeight). Times the spacing's reciprocal rather than over the spacing: a GPU divides less exactly
    * than it multiplies, and the planting reads the ground on both (scene/planting.h).
    */
    using Between = float (*)(float, float, float, float, float, float);

    float sampleGrid(const TerrainHeightGrid& grid, float x, float z, Between between) {
        const float last = static_cast<float>(grid.size - 1);
        const float inverseSpacing = 1.0f / grid.spacing;
        const float column = std::clamp(plantingCoordinate(x, grid.corner.x, inverseSpacing), 0.0f, last);
        const float row = std::clamp(plantingCoordinate(z, grid.corner.y, inverseSpacing), 0.0f, last);
        const std::uint32_t left = std::min(static_cast<std::uint32_t>(column), grid.size - 2);
        const std::uint32_t top = std::min(static_cast<std::uint32_t>(row), grid.size - 2);
        const float across = column - static_cast<float>(left);
        const float down = row - static_cast<float>(top);
        const std::size_t first = static_cast<std::size_t>(top) * grid.size + left;
        return between(grid.heights[first], grid.heights[first + 1], grid.heights[first + grid.size], grid.heights[first + grid.size + 1], across, down);
    }

    // The finest grid holding a place read by between there; past every grid, the coarsest's nearest edge.
    float sampleHeights(const TerrainHeights& heights, float x, float z, Between between) {
        if (heights.grids.empty()) {
            return 0.0f;
        }

        for (const TerrainHeightGrid& grid : heights.grids) {
            if (holds(grid, x, z)) {
                return sampleGrid(grid, x, z, between);
            }
        }

        return sampleGrid(heights.grids.back(), x, z, between);
    }
}

float terrainHeight(const TerrainHeights& heights, float x, float z) {
    return sampleHeights(heights, x, z, terrainBilinear);
}

float terrainTriangleHeight(float nearLeft, float nearRight, float farLeft, float farRight, float across, float down) {
    if (across + down <= 1.0f) {
        return nearLeft + (nearRight - nearLeft) * across + (farLeft - nearLeft) * down;
    }

    return farRight + (farLeft - farRight) * (1.0f - across) + (nearRight - farRight) * (1.0f - down);
}

float terrainSurfaceHeight(const TerrainHeights& heights, float x, float z) {
    return sampleHeights(heights, x, z, terrainTriangleHeight);
}

TerrainHeightRange terrainHeightRange(const TerrainHeights& heights, float west, float east, float north, float south) {
    TerrainHeightRange range{std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};

    for (const TerrainHeightGrid& grid : heights.grids) {
        if (grid.pyramid.size() != 2 * terrainPyramidLevelStart(grid.size, terrainPyramidLevels(grid.size) + 1)) {
            throw std::invalid_argument("A terrain height grid's range is read from its pyramid, built with it");
        }

        const float last = static_cast<float>(grid.size - 1);
        const auto point = [&](float place, float corner, bool upper) {
            const float coordinate = std::clamp((place - corner) / grid.spacing, 0.0f, last);
            return static_cast<std::uint32_t>(upper ? std::ceil(coordinate) : std::floor(coordinate));
        };

        // One point wider each way: a place's squares are found by its rounded coordinate, which may slip a square.
        const std::uint32_t firstColumn = std::max(point(west, grid.corner.x, false), 1u) - 1u;
        const std::uint32_t lastColumn = std::min(point(east, grid.corner.x, true) + 1u, grid.size - 1u);
        const std::uint32_t firstRow = std::max(point(north, grid.corner.y, false), 1u) - 1u;
        const std::uint32_t lastRow = std::min(point(south, grid.corner.y, true) + 1u, grid.size - 1u);

        // The finest level where the points span at most two texels each way: its four texels hold them all.
        int level = 0;

        while ((firstColumn >> level) + 1u < (lastColumn >> level) || (firstRow >> level) + 1u < (lastRow >> level)) {
            ++level;
        }

        for (const std::uint32_t row : {firstRow >> level, lastRow >> level}) {
            for (const std::uint32_t column : {firstColumn >> level, lastColumn >> level}) {
                const TerrainHeightRange texel = terrainPyramidTexel(grid, level, column, row);
                range.lowest = std::min(range.lowest, texel.lowest);
                range.highest = std::max(range.highest, texel.highest);
            }
        }

        // A grid holding the whole box is the finest holding each place in it: no coarser one is read there.
        if (holds(grid, west, north) && holds(grid, east, south)) {
            break;
        }
    }

    if (range.lowest > range.highest) {
        return TerrainHeightRange{};
    }

    // Interpolation rounds: at a corner it can land a last bit past the corner's own height.
    const float slack = 1e-5f * std::max({1.0f, std::abs(range.lowest), std::abs(range.highest)});
    return TerrainHeightRange{range.lowest - slack, range.highest + slack};
}

Vec3 terrainNormal(const TerrainHeights& heights, float x, float z, float step) {
    const float slopeX = (terrainHeight(heights, x + step, z) - terrainHeight(heights, x - step, z)) / (2.0f * step);
    const float slopeZ = (terrainHeight(heights, x, z + step) - terrainHeight(heights, x, z - step)) / (2.0f * step);
    return Vec3{-slopeX, 1.0f, -slopeZ}.normalized();
}

Vec4 terrainClusterBox(const TerrainPatch& patch, const Vec4& footprint) {
    const float west = std::max(std::floor(footprint.x * patch.quads) - 1.0f, 0.0f);
    const float north = std::max(std::floor(footprint.y * patch.quads) - 1.0f, 0.0f);
    const float east = std::min(std::ceil(footprint.z * patch.quads), patch.quads);
    const float south = std::min(std::ceil(footprint.w * patch.quads), patch.quads);
    return Vec4{patch.corner.x + west * patch.spacing, patch.corner.y + north * patch.spacing, patch.corner.x + east * patch.spacing,
                patch.corner.y + south * patch.spacing};
}

Vec4 terrainBoxSphere(const Vec4& box, const TerrainHeightRange& heights) {
    const Vec3 low{box.x, heights.lowest, box.y};
    const Vec3 high{box.z, heights.highest, box.w};
    const Vec3 centre = (low + high) * 0.5f;
    const Vec3 half = (high - low) * 0.5f;
    const float reach = std::max({std::abs(low.x), std::abs(low.y), std::abs(low.z), std::abs(high.x), std::abs(high.y), std::abs(high.z)});
    return Vec4{centre.x, centre.y, centre.z, std::sqrt(half.dot(half)) + 1e-5f * reach};
}

float terrainMorph(float distance, float start, float end) {
    return std::clamp((distance - start) / (end - start), 0.0f, 1.0f);
}

// fract(g / 2) * 2 is 1 for an odd coordinate and 0 for an even one.
Vec2 morphGridCoordinate(const Vec2& grid, float morph) {
    const auto slide = [morph](float coordinate) {
        const float half = coordinate * 0.5f;
        return coordinate - (half - std::floor(half)) * 2.0f * morph;
    };
    return Vec2{slide(grid.x), slide(grid.y)};
}

/*
* The mesh's points are i / quads, so the grid coordinate is rounded back to the whole number it stands for: a float a
* hair below 3 would read as odd's neighbour and slide the wrong way.
*/
TerrainVertex terrainVertex(const TerrainHeights& heights, const TerrainPatch& patch, const Vec2& meshPoint, const Vec3& eye) {
    const Vec2 grid{std::round(meshPoint.x * patch.quads), std::round(meshPoint.y * patch.quads)};
    const float startX = patch.corner.x + grid.x * patch.spacing;
    const float startZ = patch.corner.y + grid.y * patch.spacing;
    const Vec3 start{startX, terrainHeight(heights, startX, startZ), startZ};
    const float morph = terrainMorph((start - eye).length(), patch.morphStart, patch.morphEnd);
    const Vec2 slid = morphGridCoordinate(grid, morph);
    const float x = patch.corner.x + slid.x * patch.spacing;
    const float z = patch.corner.y + slid.y * patch.spacing;
    const float step = patch.spacing * (1.0f + morph);

    TerrainVertex vertex;
    vertex.position = Vec3{x, terrainHeight(heights, x, z), z};
    vertex.normal = terrainNormal(heights, x, z, step);
    vertex.tangent = Vec3{2.0f * step, terrainHeight(heights, x + step, z) - terrainHeight(heights, x - step, z), 0.0f}.normalized();
    vertex.tangentSign = -1.0f;
    vertex.uv = Vec2{(x - patch.uvCorner.x) * patch.uvPerMetre, (z - patch.uvCorner.y) * patch.uvPerMetre};
    return vertex;
}
