#include "physics/collider_raycast.h"

#include "math/transform.h"

/*
* One pass over every collider, keeping the nearest hit so far and shrinking the reach to it, so anything farther is
* rejected by its first, cheap test. In Triangles mode a shape hit is only a candidate: the detail mesh must confirm it,
* and if the ray slips between its triangles (a hole the box covers) the entity does not count and the search goes on to
* whatever is behind. Skipping an entity whose shape is out of reach is safe because the shape encloses the triangles: the
* ray enters the shape no later than it reaches any triangle, so the shape's distance is a lower bound for theirs.
*/
std::optional<ColliderHit> raycastColliders(const World& world, const Ray& ray, RaycastDetail detail, float maxDistance) {
    std::optional<ColliderHit> best;
    float reach = maxDistance;

    world.each<Collider, Transform>([&](Entity entity, const Collider& collider, const Transform& transform) {
        const Mat4 model = transform.getMatrix();

        std::optional<RayHit> hit = raycast(ray, worldShape(collider.shape, model), reach);
        if (!hit) {
            return;
        }

        if (detail == RaycastDetail::Triangles && collider.detail) {
            hit = raycast(ray, *collider.detail, model, reach);
            if (!hit) {
                return;
            }
        }

        best = ColliderHit{entity, *hit};
        reach = hit->distance;
    });

    return best;
}
