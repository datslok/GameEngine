#pragma once

#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "math/vec3.h"
#include "physics/static_level.h"

/*
* Tuning for a particle: a small ball that flies under gravity and bounces off the level (grenades, projectiles, debris).
* Distances in metres, times in seconds. The defaults are a small, hard ball on stone.
*/
struct ParticleSettings {
    float radius = 0.1f;
    float gravity = 9.81f;      // downward acceleration
    float restitution = 0.45f;  // the share of the speed into a surface that comes back out of it: 0 sticks, 1 bounces fully
    float friction = 0.4f;      // Coulomb friction: sliding slows by at most friction times the push from the surface
    float restingSpeed = 1.0f;  // arriving slower than this into a surface does not bounce, so bouncing dies out and it rests
    int maxBounces = 4;         // surfaces met in one tick; a ball wedged in a corner stops for the rest of the tick
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(ParticleSettings& settings, SettingsVisitor& visitor);

/*
* Component: a particle. Its Transform's position is the ball's centre. velocity is its state; settings may be changed at
* any time.
*/
struct Particle {
    ParticleSettings settings;
    Vec3 velocity;
};

// How far a body moves in one step, and the velocity it ends with.
struct FlightStep {
    Vec3 displacement;
    Vec3 velocity;
};

/*
* One step of flight under a constant acceleration. The move uses the average of the starting and ending velocities,
* v t + a t^2 / 2, which is the exact solution for a constant acceleration (velocity Verlet with a constant force), so a
* thrown ball follows the true parabola however long the steps are.
*/
FlightStep fly(const Vec3& velocity, const Vec3& acceleration, float seconds);

/*
* The velocity after meeting a surface with the given unit normal (pointing out of the surface). The part going into the
* surface comes back out scaled by the restitution (or not at all below the resting speed). The sliding part is slowed by
* friction: the surface's push is (1 + e) times the speed into it, and Coulomb friction can take at most friction times
* that off the sliding speed, never turning it round. A velocity already leaving the surface is unchanged.
*/
Vec3 bounceVelocity(const Vec3& velocity, const Vec3& normal, const ParticleSettings& settings);

/*
* One tick: out of anything it overlaps, the push of whatever it rests on, then flight swept through the level so it can
* never pass through a wall, bouncing off whatever it meets on the way.
*/
void moveParticle(Particle& particle, Vec3& centre, const StaticLevel& level, float seconds);

// moveParticle for every entity with a Particle and a Transform (whose position is the centre).
void moveParticles(World& world, const StaticLevel& level, float seconds);
