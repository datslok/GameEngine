#include "physics/collider_raycast.h"

#include "physics/collision_scene.h"

/*
* A one-off query: build a scene and ask it once, so there is a single implementation of the search. For many queries
* (every tick, every character) keep a CollisionScene and rebuild it once instead.
*/
std::optional<ColliderHit> raycastColliders(const World& world, const Ray& ray, RaycastDetail detail, float maxDistance) {
    CollisionScene scene;
    scene.rebuild(world);
    return raycast(scene, ray, detail, maxDistance);
}
