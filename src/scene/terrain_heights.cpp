#include "scene/terrain_heights.h"

#include <algorithm>
#include <cmath>
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
    return grid;
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

    // The four points round a place, clamped onto the grid, interpolated by between (terrainBilinear or
    // terrainTriangleHeight).
    using Between = float (*)(float, float, float, float, float, float);

    float sampleGrid(const TerrainHeightGrid& grid, float x, float z, Between between) {
        const float last = static_cast<float>(grid.size - 1);
        const float column = std::clamp((x - grid.corner.x) / grid.spacing, 0.0f, last);
        const float row = std::clamp((z - grid.corner.y) / grid.spacing, 0.0f, last);
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

Vec3 terrainNormal(const TerrainHeights& heights, float x, float z, float step) {
    const float slopeX = (terrainHeight(heights, x + step, z) - terrainHeight(heights, x - step, z)) / (2.0f * step);
    const float slopeZ = (terrainHeight(heights, x, z + step) - terrainHeight(heights, x, z - step)) / (2.0f * step);
    return Vec3{-slopeX, 1.0f, -slopeZ}.normalized();
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
