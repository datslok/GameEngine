#pragma once

#include "ecs/world.h"
#include "physics/shapes.h"
#include "physics/static_level.h"

/*
* Tuning for a kinematic character: its capsule, how it meets slopes and steps, and how it falls and jumps. Distances in
* metres (one world unit), times in seconds. Gravity, falling and jumping default to the community-measured Overwatch
* values; the two accelerations are not published anywhere and are starting points to tune by feel.
*/
struct CharacterControllerSettings {
    float radius = 0.4f;           // the capsule's radius
    float height = 1.8f;           // feet to top of head; at least two radii

    float maxSlopeDegrees = 45.0f; // steeper surfaces are walls: not walkable, slid down when standing on them
    float stepHeight = 0.3f;       // ledges up to this high are stepped onto
    float snapDistance = 0.3f;     // drops up to this deep are followed down while walking, instead of becoming a fall

    float gravity = 17.5f;         // downward acceleration at the start of a fall
    float maxFallSpeed = 30.0f;    // terminal speed: air drag grows with speed squared and balances gravity here
    float jumpSpeed = 5.72f;       // upward speed a jump starts with (Overwatch's; peaks at speed^2 / 2g, about 0.93)

    float groundAcceleration = 100.0f; // how fast walking speed changes on the ground (also how fast it stops)
    float airAcceleration = 30.0f;     // how fast steering changes velocity in the air (no input keeps momentum)

    float substepFraction = 0.5f; // no substep moves more than this fraction of the radius, so walls cannot be skipped
    int pushOutPasses = 8;        // at most this many push-outs per substep (corners need one per wall)
};

/*
* Component: a character moved by commands and kept out of the level. Its Transform's position is its feet: the bottom
* of the capsule, on the ground when standing.
*
* Commands go in wishVelocity (the horizontal velocity it wants; any vertical part is ignored) and wantsJump (used up by
* the next tick, and only honoured on the ground). The rest is its state, read by games and the debug view.
*/
struct CharacterController {
    CharacterControllerSettings settings;

    Vec3 wishVelocity;
    bool wantsJump = false;

    Vec3 velocity;
    bool grounded = false;
    Vec3 groundNormal{0.0f, 1.0f, 0.0f};
    int lastSubsteps = 0;

    // A new controller does not know yet whether it stands on anything; its first tick looks before moving.
    bool groundKnown = false;
};

// The capsule a controller occupies with its feet at the given point: radius as set, from feet to feet + height.
Capsule controllerCapsule(const CharacterControllerSettings& settings, const Vec3& feet);

/*
* One tick of movement through the level: jump if asked and grounded, change the horizontal velocity towards the wished
* one (fast on the ground, slower in the air, keeping speed when steering up to 90 degrees in the air), fall with gravity
* and drag, then move in substeps, pushing out of what it runs into, stepping up ledges and following the ground down.
* Walking on a slope covers the same distance along the surface as on flat ground.
*/
void moveCharacter(CharacterController& controller, Vec3& feet, const StaticLevel& level, float seconds);

// Whether a character with these settings would fit with its feet there, overlapping nothing (touching is fine): what
// standing up from a crouch needs to know.
bool characterFits(const CharacterControllerSettings& settings, const Vec3& feet, const StaticLevel& level);

// moveCharacter for every entity with a CharacterController and a Transform (whose position is the feet).
void moveCharacters(World& world, const StaticLevel& level, float seconds);
