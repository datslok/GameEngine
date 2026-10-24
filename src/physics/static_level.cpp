#include "physics/static_level.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
    Aabb triangleBounds(const Vec3& a, const Vec3& b, const Vec3& c) {
        return Aabb{
            Vec3{std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}), std::min({a.z, b.z, c.z})},
            Vec3{std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y}), std::max({a.z, b.z, c.z})}
        };
    }
}

StaticLevel::StaticLevel(const CollisionSettings& settings)
    : grid(settings.gridCellSize, settings.largeItemCells) {
}

/*
* Everything is checked and moved into the world before anything is stored, so a bad piece leaves the level as it was.
* Each new triangle then goes into the grid under its own number, the order it has in the merged mesh.
*/
void StaticLevel::add(const TriangleMesh& piece, const Mat4& model) {
    if (piece.indices.size() % 3 != 0) {
        throw std::invalid_argument("A level piece's indices must come in threes");
    }
    for (const std::uint32_t index : piece.indices) {
        if (index >= piece.positions.size()) {
            throw std::invalid_argument("A level piece's triangle refers to a corner it does not have");
        }
    }

    std::vector<Vec3> placed;
    placed.reserve(piece.positions.size());
    for (const Vec3& position : piece.positions) {
        const Vec3 moved = transformPoint(model, position);
        if (!std::isfinite(moved.x) || !std::isfinite(moved.y) || !std::isfinite(moved.z)) {
            throw std::invalid_argument("A level piece's corners must be finite");
        }
        placed.push_back(moved);
    }

    const std::uint32_t firstCorner = static_cast<std::uint32_t>(mesh.positions.size());
    mesh.positions.insert(mesh.positions.end(), placed.begin(), placed.end());

    for (std::size_t corner = 0; corner < piece.indices.size(); corner += 3) {
        const std::uint32_t triangle = static_cast<std::uint32_t>(mesh.indices.size() / 3);
        for (int k = 0; k < 3; ++k) {
            mesh.indices.push_back(firstCorner + piece.indices[corner + k]);
        }
        const auto corners = getTriangle(triangle);
        grid.insert(triangle, triangleBounds(corners[0], corners[1], corners[2]));
    }
}

std::size_t StaticLevel::getTriangleCount() const {
    return mesh.indices.size() / 3;
}

std::array<Vec3, 3> StaticLevel::getTriangle(std::uint32_t triangle) const {
    const std::size_t first = static_cast<std::size_t>(triangle) * 3;
    return {mesh.positions[mesh.indices[first]], mesh.positions[mesh.indices[first + 1]], mesh.positions[mesh.indices[first + 2]]};
}

const TriangleMesh& StaticLevel::getMesh() const {
    return mesh;
}

const UniformGrid& StaticLevel::getGrid() const {
    return grid;
}

/*
* The grid walks the ray near to far and hands over each triangle in the cells it passes; a hit shrinks the reach, so the
* walk stops as soon as no later cell could hold anything nearer.
*/
std::optional<LevelHit> raycast(const StaticLevel& level, const Ray& ray, float maxDistance, CollisionQueryStats* stats) {
    std::optional<LevelHit> best;

    const GridRayWalk walk = level.getGrid().walkRay(ray, maxDistance, [&](std::uint32_t triangle, float reach) {
        const auto corners = level.getTriangle(triangle);
        const std::optional<RayHit> hit = raycastTriangle(ray, corners[0], corners[1], corners[2], reach);
        if (!hit) {
            return reach;
        }
        best = LevelHit{*hit, triangle};
        return hit->distance;
    });

    if (stats) {
        stats->tested = walk.items;
        stats->cellsWalked = walk.cells;
    }
    return best;
}

/*
* Only triangles whose boxes overlap the box around the whole sweep (the sphere at its start and at its end) can be met
* on the way; each of those is swept against exactly, and the nearest wins.
*/
std::optional<LevelHit> sweepSphere(const StaticLevel& level, const Sphere& sphere, const Vec3& motion) {
    const Aabb start = boundsOf(Shape{sphere});
    const Aabb end = boundsOf(Shape{Sphere{sphere.centre + motion, sphere.radius}});
    const Aabb swept = enclosing(start, end);
    std::vector<std::uint32_t> candidates;
    level.getGrid().query(swept, candidates);

    std::optional<LevelHit> best;
    for (const std::uint32_t triangle : candidates) {
        const auto corners = level.getTriangle(triangle);
        const std::optional<RayHit> hit = sweepSphere(sphere, motion, corners[0], corners[1], corners[2]);
        if (hit && (!best || hit->distance < best->hit.distance)) {
            best = LevelHit{*hit, triangle};
        }
    }
    return best;
}

/*
* Only triangles whose boxes overlap the capsule's box can touch it; each of those gets the exact contact test.
*/
std::vector<Contact> contacts(const StaticLevel& level, const Capsule& capsule) {
    std::vector<std::uint32_t> candidates;
    level.getGrid().query(boundsOf(Shape{capsule}), candidates);

    std::vector<Contact> found;
    for (const std::uint32_t triangle : candidates) {
        const auto corners = level.getTriangle(triangle);
        if (std::optional<Contact> touching = contact(capsule, corners[0], corners[1], corners[2])) {
            touching->triangle = triangle;
            found.push_back(*touching);
        }
    }
    return found;
}
