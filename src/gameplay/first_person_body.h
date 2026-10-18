#pragma once

#include "ecs/world.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "physics/character_controller.h"
#include "physics/static_level.h"

/*
* How a first-person body walks, from the community-measured Overwatch values (walking, backwards and crouching speeds)
* plus a sprint, and its heights standing and crouched (guesses, to tune by feel).
*/
struct FirstPersonMoveSettings {
    float walkSpeed = 5.5f;                   // forwards, sideways and the forward diagonals
    float backwardMultiplier = 0.9f;          // straight back: 4.95
    float backwardDiagonalMultiplier = 0.93333f; // back and to the side: 5.13
    float crouchSpeed = 3.0f;                 // any direction
    float sprintMultiplier = 1.5f;            // while Shift is held, not crouched

    float standingHeight = 1.8f;
    float standingEyeHeight = 1.6f;
    float crouchingHeight = 1.2f;
    float crouchingEyeHeight = 1.0f;
    float eyeHeightSpeed = 6.0f;              // how fast the eyes move between the two, in units a second (0.1 s)
};

/*
* Component: the player's own body in first person, on an entity with a Transform (the feet), a PreviousTransform and a
* CharacterController that moves it. Its command is wantsCrouch; it stays crouched while there is no room to stand.
*/
struct FirstPersonBody {
    FirstPersonMoveSettings settings;
    bool wantsCrouch = false;
    bool crouching = false;
    float eyeHeight = 1.6f; // eased towards the standing or crouching eye height
};

/*
* The horizontal velocity the movement keys ask for: axes from getMoveAxes (x right, y forward, each -1, 0 or 1) turned
* into the world by the facing yaw (0 looks down -z, positive turns right), normalised so diagonals are not faster, and
* scaled by the speed for that direction (walking, backwards, backward diagonal, crouching, sprinting).
*/
Vec3 firstPersonWishVelocity(const Vec2& axes, float yaw, bool crouching, bool sprinting, const FirstPersonMoveSettings& settings);

// Creates a standing body with its feet at the given point. The controller's height comes from the move settings.
Entity spawnFirstPersonBody(World& world, const Vec3& feet, const FirstPersonMoveSettings& settings = FirstPersonMoveSettings{},
                            const CharacterControllerSettings& controller = CharacterControllerSettings{});

// Moves the body's feet somewhere new at once: no interpolation streak, no speed carried along, ground looked up afresh.
void placeFirstPersonBody(World& world, Entity body, const Vec3& feet);

// One tick for every body: crouch or stand up (if there is room), ease the eyes, then move it through the level.
void updateFirstPersonBodies(World& world, const StaticLevel& level, float seconds);

// Where the eyes are when drawn: the interpolated feet plus the eye height.
Vec3 firstPersonEyePosition(const World& world, Entity body, float alpha);
