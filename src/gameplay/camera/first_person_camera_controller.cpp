#include "gameplay/camera/first_person_camera_controller.h"

FirstPersonCameraController::FirstPersonCameraController(float moveSpeed, float radiansPerPixel):
    moveSpeed(moveSpeed),
    radiansPerPixel(radiansPerPixel)
{
}

void FirstPersonCameraController::takeOver(const Camera& camera) {
    angles = anglesFromDirection(camera.getForward());
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
        camera.setPosition(camera.getPosition() + movement.normalized() * (moveSpeed * frameSeconds));
    }
}

const ViewAngles& FirstPersonCameraController::getAngles() const {
    return angles;
}
