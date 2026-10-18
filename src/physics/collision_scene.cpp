#include "physics/collision_scene.h"

#include "math/transform.h"
#include "physics/overlap.h"

CollisionScene::CollisionScene(const CollisionSettings& settings)
    : settings(settings), grid(settings.gridCellSize, settings.largeItemCells) {
}

/*
* Start again from nothing each time: every collider's world shape is worked out once here, so the many queries that follow
* share it, and nothing can be left over from an entity that has moved or gone. The grid is remade only if its settings
* changed; otherwise it is cleared, keeping its memory.
*/
void CollisionScene::rebuild(const World& world) {
    if (grid.getCellSize() != settings.gridCellSize || builtLargeItemCells != settings.largeItemCells) {
        grid = UniformGrid{settings.gridCellSize, settings.largeItemCells};
        builtLargeItemCells = settings.largeItemCells;
    } else {
        grid.clear();
    }
    colliders.clear();

    world.each<Collider, Transform>([&](Entity entity, const Collider& collider, const Transform& transform) {
        const Mat4 model = transform.getMatrix();
        const Shape shape = worldShape(collider.shape, model);
        grid.insert(static_cast<std::uint32_t>(colliders.size()), boundsOf(shape));
        colliders.push_back(SceneCollider{entity, shape, model, collider.detail});
    });
}

const std::vector<SceneCollider>& CollisionScene::getColliders() const {
    return colliders;
}

const UniformGrid& CollisionScene::getGrid() const {
    return grid;
}

CollisionSettings& CollisionScene::getSettings() {
    return settings;
}

const CollisionSettings& CollisionScene::getSettings() const {
    return settings;
}

/*
* The grid hands over colliders near to far along the ray; each gets the exact test (its shape, then its triangles in
* Triangles mode), and a hit shrinks the reach, which is what lets the walk stop early. A shape hit that the triangles do
* not confirm (the ray slipped through a gap) leaves the reach alone, so the walk goes on to what is behind.
*/
std::optional<ColliderHit> raycast(const CollisionScene& scene, const Ray& ray, RaycastDetail detail, float maxDistance,
                                   CollisionQueryStats* stats) {
    std::optional<ColliderHit> best;
    const std::vector<SceneCollider>& colliders = scene.getColliders();

    const GridRayWalk walk = scene.getGrid().walkRay(ray, maxDistance, [&](std::uint32_t item, float reach) {
        const SceneCollider& collider = colliders[item];

        std::optional<RayHit> hit = raycast(ray, collider.shape, reach);
        if (hit && detail == RaycastDetail::Triangles && collider.detail) {
            hit = raycast(ray, *collider.detail, collider.model, reach);
        }
        if (!hit) {
            return reach;
        }

        best = ColliderHit{collider.entity, *hit};
        return hit->distance;
    });

    if (stats) {
        stats->tested = walk.items;
        stats->cellsWalked = walk.cells;
    }
    return best;
}

/*
* The grid narrows the search to colliders whose boxes overlap the shape's box; the exact overlap test decides.
*/
std::vector<Entity> overlapping(const CollisionScene& scene, const Shape& shape) {
    std::vector<std::uint32_t> candidates;
    scene.getGrid().query(boundsOf(shape), candidates);

    std::vector<Entity> found;
    for (const std::uint32_t item : candidates) {
        const SceneCollider& collider = scene.getColliders()[item];
        if (overlaps(collider.shape, shape)) {
            found.push_back(collider.entity);
        }
    }
    return found;
}
