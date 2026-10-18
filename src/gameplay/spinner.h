#pragma once

#include "ecs/world.h"
#include "math/vec3.h"

/*
* Component: spins an entity at a constant rate. Used by the demo objects.
*/
struct Spinner {
    // Euler angles in radians (about x, then y, then z, as Quat::fromEulerAngles combines them), each growing at its speed.
    Vec3 initialAngles{0.0f, 0.0f, 0.0f};
    Vec3 speed{0.0f, 0.0f, 0.0f}; // Radians per second.
};

// System: set each spinner's rotation for the given simulation time, replacing the whole Transform rotation.
void updateSpinners(World& world, double simulationSeconds);
