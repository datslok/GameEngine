#include "gameplay/camera/first_person_camera_controller.h"

#include <cmath>

/*
* A thrown ball rises until gravity has used up its speed: v^2 = 2 g h, so the launch speed for a chosen height is sqrt(2 g h).
* Setting the height and gravity (rather than the speed) keeps the jump's feel easy to tune.
*/
FirstPersonCameraController::FirstPersonCameraController(float moveSpeed, float radiansPerPixel, float jumpHeight, float gravity):
    moveSpeed(moveSpeed),
    radiansPerPixel(radiansPerPixel),
    gravity(gravity),
    jumpSpeed(std::sqrt(2.0f * gravity * jumpHeight))
{
}

void FirstPersonCameraController::takeOver(const Camera& camera) {
    angles = anglesFromDirection(camera.getForward());
    eyeHeight = camera.getPosition().y;
    verticalSpeed = 0.0f;
    airborne = false;
}

bool FirstPersonCameraController::isAirborne() const {
    return airborne;
}

/*
* Constant gravity has an exact answer for any length of time: y += v t - g t^2 / 2 and v -= g t. Using it instead of a
* step-by-step approximation makes the jump the same parabola at any frame rate. Reaching the eye height again is landing.
* Space only jumps from the ground, so pressing it in mid-air does nothing.
*/
void FirstPersonCameraController::updateJump(Camera& camera, const Input& input, float frameSeconds) {
    if (!airborne && input.wasKeyPressed(Key::Space)) {
        airborne = true;
        verticalSpeed = jumpSpeed;
    }

    if (!airborne) {
        return;
    }

    Vec3 position = camera.getPosition();
    position.y += verticalSpeed * frameSeconds - 0.5f * gravity * frameSeconds * frameSeconds;
    verticalSpeed -= gravity * frameSeconds;

    if (position.y <= eyeHeight && verticalSpeed < 0.0f) {
        position.y = eyeHeight;
        verticalSpeed = 0.0f;
        airborne = false;
    }

    camera.setPosition(position);
}

/*
* Walking uses only the yaw: the same angles with zero pitch give a level forward direction.
*/
void FirstPersonCameraController::update(Camera& camera, const Input& input, float frameSeconds) {
    if (!input.isMouseCaptured()) {
        return;
    }

    applyMouseLook(angles, input.getMouseDelta(), radiansPerPixel);
    camera.setForward(directionFromAngles(angles));

    const Vec2 axes = getMoveAxes(input);
    const Vec3 levelForward = directionFromAngles(ViewAngles{angles.yaw, 0.0f});
    const Vec3 levelRight = levelForward.cross(Vec3{0.0f, 1.0f, 0.0f});

    const Vec3 movement = levelForward * axes.y + levelRight * axes.x;

    // Normalizing keeps diagonal movement from being faster.
    if (movement.lengthSquared() > 0.0f) {
        camera.setPosition(camera.getPosition() + movement.normalized() * (moveSpeed * getSpeedMultiplier(input) * frameSeconds));
    }

    updateJump(camera, input, frameSeconds);
}

const ViewAngles& FirstPersonCameraController::getAngles() const {
    return angles;
}
