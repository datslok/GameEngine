#include "gameplay/grenade.h"
#include "math/transform.h"
#include "physics/collider.h"
#include "scene/interpolation.h"
#include "scene/model_renderer.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <variant>

namespace {
    constexpr float tick = 1.0f / 120.0f;

    // A thrown grenade is a small sphere at the throw point, flying along the (normalised) direction at the throw speed,
    // drawn with the given looks, with a lifetime, a sphere collider and a matching previous pose for interpolation.
    void testThrow() {
        World world;
        GrenadeSettings settings;
        settings.throwSpeed = 12.0f;
        settings.lifetimeSeconds = 3.0f;
        settings.particle.radius = 0.2f;
        const ModelRenderer looks = makeMeshRenderer(MeshHandle{4});

        const Entity grenade = throwGrenade(world, Vec3{1.0f, 2.0f, 3.0f}, Vec3{0.0f, 0.0f, -2.0f}, looks, settings);
        assert(world.isAlive(grenade));

        const Transform& transform = world.get<Transform>(grenade);
        assert(near(transform.position.x, 1.0f, 1e-6f) && near(transform.position.y, 2.0f, 1e-6f) && near(transform.position.z, 3.0f, 1e-6f));
        assert(near(transform.scale.x, 0.2f, 1e-6f) && near(transform.scale.y, 0.2f, 1e-6f) && near(transform.scale.z, 0.2f, 1e-6f));
        assert(near(world.get<PreviousTransform>(grenade).transform.position.z, 3.0f, 1e-6f));

        const Particle& particle = world.get<Particle>(grenade);
        assert(near(particle.velocity.z, -12.0f, 1e-5f) && near(particle.velocity.x, 0.0f, 1e-6f));
        assert(near(particle.settings.radius, 0.2f, 1e-6f));
        assert(near(world.get<Grenade>(grenade).secondsLeft, 3.0f, 1e-6f));
        assert(world.get<ModelRenderer>(grenade).parts.size() == 1);

        // A unit sphere in its own coordinates; the transform's scale makes it the particle's size.
        const Sphere* shape = std::get_if<Sphere>(&world.get<Collider>(grenade).shape);
        assert(shape && near(shape->radius, 1.0f, 1e-6f));
    }

    // Grenades last their lifetime and are then removed; others are untouched.
    void testLifetime() {
        World world;
        GrenadeSettings shortLived;
        shortLived.lifetimeSeconds = 0.5f;
        GrenadeSettings longLived;
        longLived.lifetimeSeconds = 2.0f;
        const ModelRenderer looks = makeMeshRenderer(MeshHandle{0});
        const Entity first = throwGrenade(world, Vec3{}, Vec3{1.0f, 0.0f, 0.0f}, looks, shortLived);
        const Entity second = throwGrenade(world, Vec3{}, Vec3{1.0f, 0.0f, 0.0f}, looks, longLived);

        for (int i = 0; i < 59; ++i) {
            updateGrenades(world, tick);
        }
        assert(world.isAlive(first) && world.isAlive(second));
        updateGrenades(world, tick);
        updateGrenades(world, tick);
        assert(!world.isAlive(first) && world.isAlive(second));
    }

    // Thrown forward and up over a floor, a grenade lands, slides to a stop, and lies on the floor.
    void testLandsAndStops() {
        const StaticLevel level = flatLevel();
        World world;
        const Entity grenade = throwGrenade(world, Vec3{0.0f, 1.6f, 0.0f}, Vec3{0.0f, 0.5f, -1.0f}, makeMeshRenderer(MeshHandle{0}));
        const float radius = world.get<Particle>(grenade).settings.radius;

        for (int i = 0; i < 8 * 120; ++i) {
            moveParticles(world, level, tick);
        }
        const Vec3 rest = world.get<Transform>(grenade).position;
        assert(near(rest.y, radius, 2e-3f));
        assert(rest.z < -5.0f);
        assert(world.get<Particle>(grenade).velocity.length() < 1e-6f);
    }
}

void testGrenade() {
    testThrow();
    testLifetime();
    testLandsAndStops();
}
