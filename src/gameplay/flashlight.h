#pragma once

#include "ecs/world.h"
#include "math/vec3.h"
#include "scene/camera.h"
#include "scene/light.h"

/*
* A flashlight a game can give its player (the demo's and the light lab's F key): a spotlight on its own entity,
* switched on and off, and placed every frame (after the camera moves, since the camera moves per frame: placed per
* tick, the beam would lag behind mouse look). It needs no PreviousTransform: it is placed exactly where things are
* drawn this frame.
*/

// Where a flashlight is and which way it shines (the beam need not be unit length).
struct FlashlightPose {
    Vec3 position;
    Vec3 direction;
};

// Held in the right hand: this far right of and below the eye, aimed at the point this far straight ahead (where a
// crosshair would be). Being off to the side, its shadows show beside things instead of hiding behind them.
inline constexpr float flashlightHandRight = 0.4f;
inline constexpr float flashlightHandDown = 0.3f;
inline constexpr float flashlightAimDistance = 8.0f;

FlashlightPose flashlightInHand(const Camera& camera);

// Warm, like an incandescent torch. A large source radius keeps it gentle up close; the higher intensity carries it further.
// Fast: it swings with the view, so its bounce light must follow it closely.
inline const SpotLight flashlightBeam{
    .colour = Vec3{1.0f, 0.89f, 0.69f},
    .intensity = 12.0f,
    .range = 50.0f,
    .sourceRadius = 4.5f,
    .fast = true
};

class Flashlight {
public:
    // Make its entity, switched on or off.
    void create(World& world, bool on);

    // Off removes the light rather than dimming it to zero, so the renderer does not spend a seat on it.
    void toggle(World& world);
    bool isOn(const World& world) const;

    // Move it, and aim the beam when it is on; switched off it still follows, so it comes back on where it should be.
    void place(World& world, const FlashlightPose& pose);

    Entity getEntity() const;

private:
    Entity entity;
    Vec3 direction{0.0f, 0.0f, -1.0f};
};
