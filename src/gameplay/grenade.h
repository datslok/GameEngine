#pragma once

#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "math/vec3.h"
#include "physics/particle.h"
#include "scene/model_renderer.h"

/*
* Tuning for thrown grenades: how hard they are thrown, how long they last, and how they fly and bounce.
*/
struct GrenadeSettings {
    float throwSpeed = 15.0f;      // metres per second along the throw direction
    float lifetimeSeconds = 5.0f;  // removed after this long (there is no explosion yet)
    ParticleSettings particle;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(GrenadeSettings& settings, SettingsVisitor& visitor);

// Component: a grenade, counting down to its removal.
struct Grenade {
    float secondsLeft = 0.0f;
};

/*
* Throws a grenade from a point along a direction (any length) at the throw speed: an entity with a Transform at the
* point, scaled to the particle's radius, a matching PreviousTransform, the given looks (a unit sphere), a unit sphere
* Collider, a Particle and a Grenade. The thrower's own velocity is not added, as in most shooters, so throws are
* predictable.
*/
Entity throwGrenade(World& world, const Vec3& from, const Vec3& direction, const ModelRenderer& looks,
                    const GrenadeSettings& settings = GrenadeSettings{});

// Counts every grenade's lifetime down and removes those whose time is up. Moving them is moveParticles' job.
void updateGrenades(World& world, float seconds);
