#pragma once

#include "ecs/world.h"
#include "math/vec3.h"
#include "scene/camera.h"
#include "scene/light.h"

class SettingsVisitor;

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

// Aimed at the point this far straight ahead of the eye (where a crosshair would be), wherever the hand holds it.
inline constexpr float flashlightAimDistance = 8.0f;

struct FlashlightSettings;

// Held in the right hand, settings.handRight right of and settings.handDown below the eye, aimed straight ahead.
FlashlightPose flashlightInHand(const Camera& camera, const FlashlightSettings& settings);

// Warm, like an incandescent torch. A large source radius keeps it gentle up close; the higher intensity carries it further.
// Fast: it swings with the view, so its bounce light must follow it closely.
inline const SpotLight flashlightBeam{
    .colour = Vec3{1.0f, 0.89f, 0.69f},
    .intensity = 12.0f,
    .range = 50.0f,
    .sourceRadius = 4.5f,
    .fast = true
};

/*
* The player's flashlight (F) as a setting too, so the set command switches it for scripts and AIs. Off at the start.
* The hand's offset from the eye, in metres: off to the side, shadows show beside things instead of hiding behind them,
* but by the angle between eye and lamp, which at 0.4 m right and 0.3 m down threw a leaf's shadow about 28 degrees aside
* at a metre and read as misplaced; close to the eye (about 10 degrees there) keeps a little depth (the user, Nov 19 2026).
*/
struct FlashlightSettings {
    bool on = false;
    float handRight = 0.15f;
    float handDown = 0.1f;
};

void visit(FlashlightSettings& settings, SettingsVisitor& visitor);

class Flashlight {
public:
    // Make its entity, switched on or off.
    void create(World& world, bool on);

    // Off removes the light rather than dimming it to zero, so the renderer does not spend a seat on it. Either way the
    // switch is a scene event for the light probes within the beam's reach (signalProbeEvent).
    void toggle(World& world);
    bool isOn(const World& world) const;

    // Move it, and aim the beam when it is on; switched off it still follows, so it comes back on where it should be.
    void place(World& world, const FlashlightPose& pose);

    Entity getEntity() const;

private:
    Entity entity;
    Vec3 direction{0.0f, 0.0f, -1.0f};
};
