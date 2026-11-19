#pragma once

#include "ecs/world.h"
#include "math/vec3.h"

/*
* Beacons: small, very bright lights seen from far off, an aircraft warning light on a tower above all (docs/research/
* STRUCTURES.md, "Planned: obstruction lights"). Drawn as the stars are, a splat keeping the light's energy at any
* distance (render/gpu/beacons.h), so a 2000 cd lamp still shows kilometres away without growing; and optionally
* throwing a little of its light on what is round it (its spill) as an ordinary point light on its entity.
*/

/*
* How a beacon flashes: on for onSeconds from the start of each period, ramping up and down over rampSeconds (an LED's
* quick edges), dark the rest of it, the periods shifted by phaseSeconds. FAA L-864 by default: 30 flashes a minute,
* about 0.5 s on. A period of 0 or less is a steady light.
*/
struct BeaconFlash {
    float periodSeconds = 2.0f;
    float onSeconds = 0.5f;
    float rampSeconds = 0.05f;
    float phaseSeconds = 0.0f;
};

// How bright a beacon flashing so is at a time on the game's clock, 0 to 1.
float beaconLevel(const BeaconFlash& flash, double seconds);

/*
* Component: a beacon, where the entity's Transform puts it. Its colour (linear) and intensity, the radius of its lens
* (the part that glows, which decides when it stops being a point), its flash, and its spill: the intensity and reach of
* the light it throws on its surroundings (none at 0). level is how bright it is now (flashBeacons sets it).
*/
struct Beacon {
    Vec3 colour{1.0f, 0.06f, 0.02f};
    float intensity = 1.0f;
    float lensRadius = 0.15f;
    BeaconFlash flash{};
    float spill = 0.0f;
    float spillRange = 20.0f;
    float level = 1.0f;
};

/*
* Every beacon's level at this time on the game's clock (seconds, the simulation's, interpolated to the frame), and its
* spill as an unshadowed, fast point light on its entity at the spill times the level; one whose spill is 0 loses it.
* Called by a game every frame.
*/
void flashBeacons(World& world, double seconds);

// A beacon as the frame draws it: where it is, its light (colour times intensity times level) and its lens's radius.
struct PlacedBeacon {
    Vec3 position;
    Vec3 light;
    float lensRadius = 0.0f;
};
