#pragma once

#include "ecs/world.h"
#include "math/mat4.h"
#include "physics/collider_raycast.h"
#include "physics/collision_settings.h"
#include "physics/uniform_grid.h"

#include <memory>
#include <optional>
#include <vector>

// One collider as the scene saw it at its last rebuild: in the world, ready to test.
struct SceneCollider {
    Entity entity;
    Shape shape;  // already moved into the world
    Mat4 model;   // the entity's model matrix, for the detail mesh
    std::shared_ptr<const TriangleMesh> detail;
};

/*
* A snapshot of every Collider with a Transform, indexed by a uniform grid, so many queries can share one build. Rebuilt
* from scratch (rebuild), typically once per tick: simple and never stale, at a cost that grows with the number of
* colliders. Queries see the world as it was at the last rebuild.
*/
class CollisionScene {
public:
    explicit CollisionScene(const CollisionSettings& settings = CollisionSettings{});

    void rebuild(const World& world);

    const std::vector<SceneCollider>& getColliders() const;
    const UniformGrid& getGrid() const;

    CollisionSettings& getSettings();
    const CollisionSettings& getSettings() const;

private:
    CollisionSettings settings;
    UniformGrid grid;
    int builtLargeItemCells = settings.largeItemCells;
    std::vector<SceneCollider> colliders;
};

// What a query cost: how many colliders the exact tests ran on, and how many grid cells a ray walked.
struct CollisionQueryStats {
    int collidersTested = 0;
    int cellsWalked = 0;
};

// The nearest collider the ray meets within maxDistance, like raycastColliders, through the scene's grid. stats, when
// given, receives what the query cost.
std::optional<ColliderHit> raycast(const CollisionScene& scene, const Ray& ray, RaycastDetail detail,
                                   float maxDistance = unlimitedRayDistance, CollisionQueryStats* stats = nullptr);

// (raycastColliders in physics/collider_raycast builds a scene and asks it once.)

// Every entity whose collider shape overlaps the given shape (shapes only; detail meshes are not consulted yet).
std::vector<Entity> overlapping(const CollisionScene& scene, const Shape& shape);
