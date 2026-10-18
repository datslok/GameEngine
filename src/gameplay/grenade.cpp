#include "gameplay/grenade.h"

#include "math/transform.h"
#include "physics/collider.h"
#include "scene/interpolation.h"

#include <vector>

/*
* The looks and the collider are a unit sphere, scaled by the Transform to the particle's radius, so one sphere mesh
* serves every size. The PreviousTransform starts equal to the Transform, so the first drawn frame does not blend in from
* the origin.
*/
Entity throwGrenade(World& world, const Vec3& from, const Vec3& direction, const ModelRenderer& looks, const GrenadeSettings& settings) {
    const float radius = settings.particle.radius;
    Transform placement;
    placement.position = from;
    placement.scale = Vec3{radius, radius, radius};

    Particle particle;
    particle.settings = settings.particle;
    if (direction.lengthSquared() > 0.0f) {
        particle.velocity = direction.normalized() * settings.throwSpeed;
    }

    const Entity grenade = world.create();
    world.add(grenade, placement);
    world.add(grenade, PreviousTransform{placement});
    world.add(grenade, looks);
    world.add(grenade, Collider{Sphere{Vec3{}, 1.0f}, nullptr});
    world.add(grenade, particle);
    world.add(grenade, Grenade{settings.lifetimeSeconds});
    return grenade;
}

// Entities cannot be destroyed inside each(), so the expired ones are collected first.
void updateGrenades(World& world, float seconds) {
    std::vector<Entity> expired;
    world.each<Grenade>([&](Entity entity, Grenade& grenade) {
        grenade.secondsLeft -= seconds;
        if (grenade.secondsLeft <= 0.0f) {
            expired.push_back(entity);
        }
    });
    for (const Entity entity : expired) {
        world.destroy(entity);
    }
}
