#pragma once

#include "ecs/world.h"
#include "math/vec3.h"

/*
* Component: makes an entity drift round a point like a firefly: a circle across the ground whose radius, height and speed
* wobble. The demo's lamps circle their pillars with it, so the pillars' shadows sweep round.
*/
struct Firefly {
    Vec3 centre{0.0f, 0.0f, 0.0f}; // The point it circles; height is measured from here.
    float orbitRadius = 1.0f;      // Average distance from the centre across the ground.
    float radiusWobble = 0.0f;     // It drifts up to this much closer or further. Keep it below orbitRadius.
    float height = 1.0f;           // Average height above the centre.
    float heightWobble = 0.0f;     // It bobs up to this much higher or lower.
    float orbitSpeed = 0.5f;       // Radians per second round the centre, on average; negative goes the other way.
    float phase = 0.0f;            // Starting angle, also shifting the wobbles, so fireflies with different phases fly out of step.
};

// Where the firefly is after the given simulation time.
Vec3 fireflyPosition(const Firefly& firefly, double seconds);

// System: move each firefly's Transform to its position for the given simulation time.
void updateFireflies(World& world, double simulationSeconds);
