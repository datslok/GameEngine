#include "scene/ground_follow.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
    // The most lines a sphere's square is read along each way: past it, evenly spaced lines this many.
    constexpr int mostLines = 129;

    // The places along one axis of a square to read the land at: its two edges, and every grid line between them (lines
    // at origin + k spacing), or mostLines evenly when there are more.
    std::vector<float> readingLines(float from, float to, float origin, float spacing) {
        std::vector<float> lines{from};
        const float first = std::ceil((from - origin) / spacing);
        const float last = std::floor((to - origin) / spacing);

        if (last - first + 3.0f > static_cast<float>(mostLines)) {
            lines.clear();

            for (int line = 0; line < mostLines; ++line) {
                lines.push_back(from + (to - from) * static_cast<float>(line) / static_cast<float>(mostLines - 1));
            }

            return lines;
        }

        for (float line = first; line <= last; line += 1.0f) {
            const float place = origin + line * spacing;

            if (place > from && place < to) {
                lines.push_back(place);
            }
        }

        lines.push_back(to);
        return lines;
    }

    // The finest grid holding a place (the one terrainHeight reads there), else the coarsest.
    const TerrainHeightGrid& gridAt(const TerrainHeights& heights, float x, float z) {
        for (const TerrainHeightGrid& grid : heights.grids) {
            const float extent = static_cast<float>(grid.size - 1) * grid.spacing;

            if (x >= grid.corner.x && z >= grid.corner.y && x <= grid.corner.x + extent && z <= grid.corner.y + extent) {
                return grid;
            }
        }

        return heights.grids.back();
    }
}

Vec3 liftedOntoGround(const TerrainHeights& heights, const Vec3& placed) {
    const float lift = heights.grids.empty() ? 0.0f : terrainHeight(heights, placed.x, placed.z);
    return Vec3{placed.x, placed.y + lift, placed.z};
}

BoundingSphere liftedSphere(const BoundingSphere& sphere, const TerrainHeights& heights) {
    if (sphere.radius < 0.0f || heights.grids.empty()) {
        return sphere;
    }

    const Vec3& centre = sphere.centre;
    const TerrainHeightGrid& grid = gridAt(heights, centre.x, centre.z);
    const std::vector<float> xs = readingLines(centre.x - sphere.radius, centre.x + sphere.radius, grid.corner.x, grid.spacing);
    const std::vector<float> zs = readingLines(centre.z - sphere.radius, centre.z + sphere.radius, grid.corner.y, grid.spacing);
    float lowest = terrainHeight(heights, xs.front(), zs.front());
    float highest = lowest;

    for (const float x : xs) {
        for (const float z : zs) {
            const float height = terrainHeight(heights, x, z);
            lowest = std::min(lowest, height);
            highest = std::max(highest, height);
        }
    }

    return BoundingSphere{Vec3{centre.x, centre.y + 0.5f * (lowest + highest), centre.z}, sphere.radius + 0.5f * (highest - lowest)};
}

void liftGroundDraws(std::span<DrawItem> draws, const TerrainHeights* heights) {
    if (heights == nullptr) {
        return;
    }

    for (DrawItem& draw : draws) {
        if (draw.followsGround) {
            draw.bounds = liftedSphere(draw.bounds, *heights);
            draw.levelBounds = liftedSphere(draw.levelBounds, *heights);
        }
    }
}
