#pragma once

#include "ecs/world.h"
#include "math/ray.h"
#include "physics/collider.h"
#include "physics/raycast.h"

#include <optional>

// How exactly a query looks at colliders.
enum class RaycastDetail {
    Shapes,    // only the coarse shapes: one cheap test per entity, for frequent gameplay queries
    Triangles, // confirm each shape hit against the collider's detail mesh, when it has one: exact, for clicks
};

// What a ray hit among the colliders, and where.
struct ColliderHit {
    Entity entity;
    RayHit hit;
};

/*
* The nearest entity with a Collider and a Transform that the ray meets within maxDistance, or nothing. Uses each entity's
* current Transform, not the interpolated pose on screen (they differ by at most one tick of motion). Tests every collider:
* the broad phase will cut that down later.
*/
std::optional<ColliderHit> raycastColliders(const World& world, const Ray& ray, RaycastDetail detail,
                                            float maxDistance = unlimitedRayDistance);
