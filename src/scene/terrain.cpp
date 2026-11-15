#include "scene/terrain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

void visit(TerrainSettings& settings, SettingsVisitor& visitor) {
    visitor.field("First range", settings.firstRangeMetres, {.min = 10.0f, .max = 10000.0f, .sliderMin = 50.0f, .sliderMax = 1000.0f, .unit = "m"});
    visitor.field("Morph share", settings.morphShare, {.min = 0.01f, .max = 1.0f});
}

void validateTerrain(const TerrainSettings& settings) {
    if (!(settings.firstRangeMetres > 0.0f) || !(settings.morphShare > 0.0f && settings.morphShare <= 1.0f)) {
        throw std::invalid_argument("Terrain needs a first range above 0 and a morph share in 0..1");
    }
}

namespace {
    int nodesAcross(const Terrain& terrain, int level) {
        return 1 << (terrain.levels - 1 - level);
    }

    float nodeSize(const Terrain& terrain, int level) {
        return terrain.leafSize * static_cast<float>(1 << level);
    }

    Vec2 rootCorner(const Terrain& terrain) {
        const float half = 0.5f * nodeSize(terrain, terrain.levels - 1);
        return Vec2{terrain.centre.x - half, terrain.centre.y - half};
    }
}

TerrainBounds makeTerrainBounds(const Terrain& terrain, JobSystem& jobs) {
    if (!terrain.heights || terrain.levels < 1 || !(terrain.leafSize > 0.0f)) {
        throw std::invalid_argument("Terrain bounds need heights, a level and a leaf size");
    }

    const TerrainHeights& heights = *terrain.heights;
    const int leaves = nodesAcross(terrain, 0);
    const Vec2 corner = rootCorner(terrain);
    TerrainBounds bounds;
    bounds.lowest.resize(static_cast<std::size_t>(terrain.levels));
    bounds.highest.resize(static_cast<std::size_t>(terrain.levels));
    bounds.lowest[0].resize(static_cast<std::size_t>(leaves) * static_cast<std::size_t>(leaves));
    bounds.highest[0].resize(bounds.lowest[0].size());

    // Each leaf: its corners' heights as read, and every grid point inside it (a coarser grid's points where a finer
    // grid reads may widen it a little, never narrow it).
    jobs.parallelFor(static_cast<std::size_t>(leaves), 1, [&](std::size_t row) {
        for (int column = 0; column < leaves; ++column) {
            const float x0 = corner.x + static_cast<float>(column) * terrain.leafSize;
            const float z0 = corner.y + static_cast<float>(row) * terrain.leafSize;
            const float x1 = x0 + terrain.leafSize;
            const float z1 = z0 + terrain.leafSize;
            float lowest = std::numeric_limits<float>::max();
            float highest = std::numeric_limits<float>::lowest();

            for (const Vec2& place : {Vec2{x0, z0}, Vec2{x1, z0}, Vec2{x0, z1}, Vec2{x1, z1}}) {
                const float height = terrainHeight(heights, place.x, place.y);
                lowest = std::min(lowest, height);
                highest = std::max(highest, height);
            }

            for (const TerrainHeightGrid& grid : heights.grids) {
                const int first = std::max(0, static_cast<int>(std::ceil((x0 - grid.corner.x) / grid.spacing)));
                const int last = std::min(static_cast<int>(grid.size) - 1, static_cast<int>(std::floor((x1 - grid.corner.x) / grid.spacing)));
                const int top = std::max(0, static_cast<int>(std::ceil((z0 - grid.corner.y) / grid.spacing)));
                const int bottom = std::min(static_cast<int>(grid.size) - 1, static_cast<int>(std::floor((z1 - grid.corner.y) / grid.spacing)));

                for (int gridRow = top; gridRow <= bottom; ++gridRow) {
                    for (int gridColumn = first; gridColumn <= last; ++gridColumn) {
                        const float height = grid.heights[static_cast<std::size_t>(gridRow) * grid.size + static_cast<std::size_t>(gridColumn)];
                        lowest = std::min(lowest, height);
                        highest = std::max(highest, height);
                    }
                }
            }

            bounds.lowest[0][row * static_cast<std::size_t>(leaves) + static_cast<std::size_t>(column)] = lowest;
            bounds.highest[0][row * static_cast<std::size_t>(leaves) + static_cast<std::size_t>(column)] = highest;
        }
    });

    for (int level = 1; level < terrain.levels; ++level) {
        const int across = nodesAcross(terrain, level);
        const int finer = across * 2;
        std::vector<float>& lowest = bounds.lowest[static_cast<std::size_t>(level)];
        std::vector<float>& highest = bounds.highest[static_cast<std::size_t>(level)];
        lowest.resize(static_cast<std::size_t>(across) * static_cast<std::size_t>(across));
        highest.resize(lowest.size());

        for (int row = 0; row < across; ++row) {
            for (int column = 0; column < across; ++column) {
                float low = std::numeric_limits<float>::max();
                float high = std::numeric_limits<float>::lowest();

                for (int child = 0; child < 4; ++child) {
                    const std::size_t index = static_cast<std::size_t>(2 * row + child / 2) * static_cast<std::size_t>(finer) +
                                              static_cast<std::size_t>(2 * column + child % 2);
                    low = std::min(low, bounds.lowest[static_cast<std::size_t>(level - 1)][index]);
                    high = std::max(high, bounds.highest[static_cast<std::size_t>(level - 1)][index]);
                }

                lowest[static_cast<std::size_t>(row) * static_cast<std::size_t>(across) + static_cast<std::size_t>(column)] = low;
                highest[static_cast<std::size_t>(row) * static_cast<std::size_t>(across) + static_cast<std::size_t>(column)] = high;
            }
        }
    }

    return bounds;
}

// Two triangles a square, both facing up: (a, a + z, a + x) and (a + x, a + z, a + x + z).
Mesh terrainGridMesh(int quads) {
    if (quads < 1) {
        throw std::invalid_argument("A terrain grid needs a quad");
    }

    Mesh mesh;
    const int across = quads + 1;
    mesh.vertices.reserve(static_cast<std::size_t>(across) * static_cast<std::size_t>(across));

    for (int row = 0; row < across; ++row) {
        for (int column = 0; column < across; ++column) {
            mesh.vertices.push_back(Vec4{static_cast<float>(column) / static_cast<float>(quads), 0.0f, static_cast<float>(row) / static_cast<float>(quads), 1.0f});
        }
    }

    const auto at = [across](int column, int row) {
        return static_cast<std::size_t>(row) * static_cast<std::size_t>(across) + static_cast<std::size_t>(column);
    };
    const auto corner = [&mesh](std::size_t vertex) {
        return Vec2{mesh.vertices[vertex].x, mesh.vertices[vertex].z};
    };

    for (int row = 0; row < quads; ++row) {
        for (int column = 0; column < quads; ++column) {
            for (const std::array<std::size_t, 3>& corners : {std::array<std::size_t, 3>{at(column, row), at(column, row + 1), at(column + 1, row)},
                                                               std::array<std::size_t, 3>{at(column + 1, row), at(column, row + 1), at(column + 1, row + 1)}}) {
                Triangle triangle{corners[0], corners[1], corners[2]};

                for (std::size_t slot = 0; slot < 3; ++slot) {
                    triangle.normals[slot] = Vec3{0.0f, 1.0f, 0.0f};
                    triangle.uvs[slot] = corner(corners[slot]);
                }

                mesh.triangles.push_back(triangle);
            }
        }
    }

    return mesh;
}

float terrainRange(int level, const TerrainSettings& settings) {
    return settings.firstRangeMetres * static_cast<float>(1 << level);
}

TerrainNode terrainNodeAt(int level, const Vec2& corner, float size, bool half, const TerrainSettings& settings) {
    const float end = terrainRange(level, settings);
    const float previous = level > 0 ? terrainRange(level - 1, settings) : 0.0f;
    return TerrainNode{.corner = corner,
                       .size = size,
                       .level = level,
                       .half = half,
                       .spacing = size / static_cast<float>(half ? TERRAIN_HALF_GRID_QUADS : TERRAIN_GRID_QUADS),
                       .morphStart = end - settings.morphShare * (end - previous),
                       .morphEnd = end};
}

namespace {
    struct Selection {
        const Terrain& terrain;
        const Vec3& eye;
        const TerrainSettings& settings;
        std::vector<TerrainNode>& nodes;

        // Whether a sphere round the eye reaches a box (the distance to its nearest point).
        bool reaches(const Vec2& corner, float size, float lowest, float highest, float range) const {
            const float dx = std::max({corner.x - eye.x, 0.0f, eye.x - (corner.x + size)});
            const float dy = std::max({lowest - eye.y, 0.0f, eye.y - highest});
            const float dz = std::max({corner.y - eye.z, 0.0f, eye.z - (corner.y + size)});
            return dx * dx + dy * dy + dz * dz <= range * range;
        }

        bool offTheLand(const Vec2& corner, float size) const {
            const float half = terrain.landHalfSize;
            return corner.x >= terrain.centre.x + half || corner.x + size <= terrain.centre.x - half || corner.y >= terrain.centre.y + half ||
                   corner.y + size <= terrain.centre.y - half;
        }

        void add(int level, const Vec2& corner, float size, bool half, float lowest, float highest) const {
            TerrainNode node = terrainNodeAt(level, corner, size, half, settings);
            node.lowest = lowest;
            node.highest = highest;
            nodes.push_back(node);
        }

        // False when the node is out of its own level's reach: its parent draws its square instead.
        bool select(int level, int column, int row) const {
            const float size = nodeSize(terrain, level);
            const Vec2 root = rootCorner(terrain);
            const Vec2 corner{root.x + static_cast<float>(column) * size, root.y + static_cast<float>(row) * size};

            if (offTheLand(corner, size)) {
                return true;
            }

            const std::size_t index = static_cast<std::size_t>(row) * static_cast<std::size_t>(nodesAcross(terrain, level)) + static_cast<std::size_t>(column);
            const float lowest = terrain.bounds->lowest[static_cast<std::size_t>(level)][index];
            const float highest = terrain.bounds->highest[static_cast<std::size_t>(level)][index];

            if (level < terrain.levels - 1 && !reaches(corner, size, lowest, highest, terrainRange(level, settings))) {
                return false;
            }

            if (level == 0 || !reaches(corner, size, lowest, highest, terrainRange(level - 1, settings))) {
                add(level, corner, size, false, lowest, highest);
                return true;
            }

            for (int child = 0; child < 4; ++child) {
                const int childColumn = 2 * column + child % 2;
                const int childRow = 2 * row + child / 2;

                if (!select(level - 1, childColumn, childRow)) {
                    const float childSize = 0.5f * size;
                    const Vec2 childCorner{root.x + static_cast<float>(childColumn) * childSize, root.y + static_cast<float>(childRow) * childSize};
                    const std::size_t childIndex = static_cast<std::size_t>(childRow) * static_cast<std::size_t>(nodesAcross(terrain, level - 1)) +
                                                   static_cast<std::size_t>(childColumn);
                    add(level, childCorner, childSize, true, terrain.bounds->lowest[static_cast<std::size_t>(level - 1)][childIndex],
                        terrain.bounds->highest[static_cast<std::size_t>(level - 1)][childIndex]);
                }
            }

            return true;
        }
    };
}

std::vector<TerrainNode> selectTerrainNodes(const Terrain& terrain, const Vec3& eye, const TerrainSettings& settings) {
    if (!terrain.heights || !terrain.bounds || static_cast<int>(terrain.bounds->lowest.size()) != terrain.levels) {
        throw std::invalid_argument("A terrain needs its heights and their bounds");
    }

    std::vector<TerrainNode> nodes;
    const Selection selection{terrain, eye, settings, nodes};
    selection.select(terrain.levels - 1, 0, 0);
    return nodes;
}

namespace {
    // The finest region holding the node whole, else the last.
    const TerrainRegion& regionOf(const Terrain& terrain, const TerrainNode& node) {
        for (const TerrainRegion& region : terrain.regions) {
            if (node.corner.x >= region.centre.x - region.halfSize && node.corner.x + node.size <= region.centre.x + region.halfSize &&
                node.corner.y >= region.centre.y - region.halfSize && node.corner.y + node.size <= region.centre.y + region.halfSize) {
                return region;
            }
        }

        return terrain.regions.back();
    }
}

void appendTerrainDraws(const Terrain& terrain, const Vec3& eye, const TerrainSettings& settings, std::vector<DrawItem>& draws,
                        std::vector<TerrainPatch>& patches) {
    if (terrain.regions.empty()) {
        throw std::invalid_argument("A terrain needs a region to take its material from");
    }

    const std::vector<TerrainNode> nodes = selectTerrainNodes(terrain, eye, settings);
    draws.reserve(draws.size() + nodes.size());
    patches.reserve(patches.size() + nodes.size());

    for (const TerrainNode& node : nodes) {
        const TerrainRegion& region = regionOf(terrain, node);
        Mat4 model = Mat4::translation(node.corner.x, 0.0f, node.corner.y);
        model.values[0][0] = node.size;
        model.values[2][2] = node.size;

        DrawItem draw;
        draw.mesh = node.half ? terrain.halfGrid : terrain.grid;
        draw.model = model;
        draw.material = region.material;
        draw.castsShadows = true;
        draw.previousModel = model;
        const float halfSize = 0.5f * node.size;
        const float halfHeight = 0.5f * (node.highest - node.lowest);
        draw.bounds = BoundingSphere{Vec3{node.corner.x + halfSize, 0.5f * (node.lowest + node.highest), node.corner.y + halfSize},
                                     std::sqrt(2.0f * halfSize * halfSize + halfHeight * halfHeight)};
        draw.terrainPatch = static_cast<int>(patches.size());
        draws.push_back(draw);

        patches.push_back(TerrainPatch{.corner = node.corner,
                                       .size = node.size,
                                       .quads = static_cast<float>(node.half ? TERRAIN_HALF_GRID_QUADS : TERRAIN_GRID_QUADS),
                                       .spacing = node.spacing,
                                       .morphStart = node.morphStart,
                                       .morphEnd = node.morphEnd,
                                       .uvCorner = Vec2{region.centre.x - region.halfSize, region.centre.y - region.halfSize},
                                       .uvPerMetre = 1.0f / (2.0f * region.halfSize)});
    }
}
