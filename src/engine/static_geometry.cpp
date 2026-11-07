#include "engine/static_geometry.h"

#include "core/hash.h"
#include "scene/collision_mesh.h"

namespace {
    // Only a triangle covering more cells than this (a floor of over 300 by 300 m at 1 m cells) goes on the list every
    // ray tests; anything smaller is listed in each cell it touches.
    constexpr int largeTriangleCells = 100000;
}

bool blocksSky(const DrawItem& draw) {
    return draw.isStatic && draw.castsShadows && !isTransparent(draw.material);
}

std::uint64_t skyBlockersFingerprint(std::span<const DrawItem> draws) {
    Fnv1aHash hash;

    for (const DrawItem& draw : draws) {
        if (!blocksSky(draw)) {
            continue;
        }

        hash.add(draw.mesh.index);

        for (const auto& row : draw.model.values) {
            for (const float value : row) {
                hash.add(value);
            }
        }
    }

    return hash.value;
}

CollisionSettings skyRayCollision() {
    CollisionSettings settings;
    settings.gridCellSize = 1.0f;
    settings.largeItemCells = largeTriangleCells;
    return settings;
}

/*
* Each blocking draw's mesh moved into the world by its matrix, the same triangles as drawn (a mesh's collision copy,
* scene/collision_mesh). A rebuild only when the fingerprint changes: static draws rarely do (at load, or when a game
* builds or removes a piece).
*/
void StaticGeometry::update(std::span<const DrawItem> draws, const AssetManager& assets) {
    const std::uint64_t now = skyBlockersFingerprint(draws);

    if (version != 0 && now == fingerprint) {
        return;
    }

    fingerprint = now;
    ++version;
    auto built = std::make_shared<StaticLevel>(skyRayCollision());
    bool any = false;

    for (const DrawItem& draw : draws) {
        if (!blocksSky(draw)) {
            continue;
        }

        built->add(collisionMesh(assets.getMesh(draw.mesh), draw.model));
        any = true;
    }

    level = any ? std::shared_ptr<const StaticLevel>{std::move(built)} : nullptr;
}

std::shared_ptr<const StaticLevel> StaticGeometry::getLevel() const {
    return level;
}

std::uint64_t StaticGeometry::getVersion() const {
    return version;
}
