#include "gameplay/camera/first_person_camera_controller.h"

#include <cmath>

FirstPersonCameraController::FirstPersonCameraController(const FirstPersonCameraSettings& settings):
    settings(settings)
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
* A thrown ball rises until gravity has used up its speed: v^2 = 2 g h, so the launch speed for a chosen height is
* sqrt(2 g h). Setting the height and gravity (rather than the speed) keeps the jump's feel easy to tune.
* Constant gravity has an exact answer for any length of time: y += v t - g t^2 / 2 and v -= g t. Using it instead of a
* step-by-step approximation makes the jump the same parabola at any frame rate. Reaching the eye height again is landing.
* Space only jumps from the ground, so pressing it in mid-air does nothing.
*/
void FirstPersonCameraController::updateJump(Camera& camera, const Input& input, float frameSeconds) {
    if (!airborne && input.wasKeyPressed(Key::Space)) {
        airborne = true;
        verticalSpeed = std::sqrt(2.0f * settings.gravity * settings.jumpHeight);
    }

    if (!airborne) {
        return;
    }

    Vec3 position = camera.getPosition();
    position.y += verticalSpeed * frameSeconds - 0.5f * settings.gravity * frameSeconds * frameSeconds;
    verticalSpeed -= settings.gravity * frameSeconds;

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

    turnCamera(camera, angles, input, settings.look);

    const Vec2 axes = getMoveAxes(input);
    const Vec3 levelForward = directionFromAngles(ViewAngles{angles.yaw, 0.0f});
    const Vec3 levelRight = levelForward.cross(Vec3{0.0f, 1.0f, 0.0f});

    moveCamera(camera, levelForward * axes.y + levelRight * axes.x, settings.moveSpeed, input, settings.look, frameSeconds);

    updateJump(camera, input, frameSeconds);
}

const ViewAngles& FirstPersonCameraController::getAngles() const {
    return angles;
}

FirstPersonCameraSettings& FirstPersonCameraController::getSettings() {
    return settings;
}
