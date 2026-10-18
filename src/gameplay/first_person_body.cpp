#include "gameplay/first_person_body.h"

#include "gameplay/camera/look_controls.h"
#include "math/transform.h"
#include "scene/interpolation.h"

#include <algorithm>

/*
* The keys give a direction in the body's own frame (forward, right); turning it by the facing yaw puts it in the world,
* using the level forward (no pitch), so looking down does not slow walking. W+D has length sqrt(2), so the direction is
* normalised before it is given a speed. With digital keys there are only eight directions, so the speed is a small table:
* full speed forwards, sideways and on the forward diagonals, less going backwards, crouching the same in every direction.
*/
Vec3 firstPersonWishVelocity(const Vec2& axes, float yaw, bool crouching, bool sprinting, const FirstPersonMoveSettings& settings) {
    if (axes.x == 0.0f && axes.y == 0.0f) {
        return Vec3{};
    }

    const Vec3 forward = directionFromAngles(ViewAngles{yaw, 0.0f});
    const Vec3 right = forward.cross(Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 direction = (forward * axes.y + right * axes.x).normalized();

    float speed = settings.walkSpeed;
    if (crouching) {
        speed = settings.crouchSpeed;
    } else {
        if (axes.y < 0.0f) {
            speed *= axes.x != 0.0f ? settings.backwardDiagonalMultiplier : settings.backwardMultiplier;
        }
        if (sprinting) {
            speed *= settings.sprintMultiplier;
        }
    }
    return direction * speed;
}

Entity spawnFirstPersonBody(World& world, const Vec3& feet, const FirstPersonMoveSettings& settings,
                            const CharacterControllerSettings& controllerSettings) {
    Transform placement;
    placement.position = feet;

    CharacterController controller;
    controller.settings = controllerSettings;
    controller.settings.height = settings.standingHeight;

    FirstPersonBody body;
    body.settings = settings;
    body.eyeHeight = settings.standingEyeHeight;

    const Entity entity = world.create();
    world.add(entity, placement);
    world.add(entity, PreviousTransform{placement});
    world.add(entity, controller);
    world.add(entity, body);
    return entity;
}

/*
* Both the current and previous transforms move, so interpolation has nothing to blend across the jump, and the ground
* is looked up again on the next tick.
*/
void placeFirstPersonBody(World& world, Entity body, const Vec3& feet) {
    Transform& transform = world.get<Transform>(body);
    transform.position = feet;
    world.get<PreviousTransform>(body).transform = transform;

    CharacterController& controller = world.get<CharacterController>(body);
    controller.velocity = Vec3{};
    controller.grounded = false;
    controller.groundKnown = false;
}

/*
* Crouching takes effect at once (the capsule only gets shorter, which always fits). Standing up needs room for the
* full-height capsule; under a low ceiling the body stays crouched until it has walked out. The eyes follow the height at
* a fixed speed, so a crouch lowers the view smoothly over a tenth of a second instead of snapping.
*/
void updateFirstPersonBodies(World& world, const StaticLevel& level, float seconds) {
    world.each<FirstPersonBody, CharacterController, Transform>(
        [&](Entity, FirstPersonBody& body, CharacterController& controller, Transform& transform) {
            const FirstPersonMoveSettings& settings = body.settings;
            if (body.wantsCrouch) {
                body.crouching = true;
            } else if (body.crouching) {
                CharacterControllerSettings standing = controller.settings;
                standing.height = settings.standingHeight;
                if (characterFits(standing, transform.position, level)) {
                    body.crouching = false;
                }
            }
            controller.settings.height = body.crouching ? settings.crouchingHeight : settings.standingHeight;

            const float targetEyeHeight = body.crouching ? settings.crouchingEyeHeight : settings.standingEyeHeight;
            const float eyeStep = settings.eyeHeightSpeed * seconds;
            body.eyeHeight = std::clamp(targetEyeHeight, body.eyeHeight - eyeStep, body.eyeHeight + eyeStep);

            moveCharacter(controller, transform.position, level, seconds);
        });
}

Vec3 firstPersonEyePosition(const World& world, Entity body, float alpha) {
    return getRenderTransform(world, body, alpha).position + Vec3{0.0f, world.get<FirstPersonBody>(body).eyeHeight, 0.0f};
}
