#include "gameplay/camera/free_fly_camera_controller.h"

FreeFlyCameraController::FreeFlyCameraController(float moveSpeed, float radiansPerPixel):
    moveSpeed(moveSpeed),
    radiansPerPixel(radiansPerPixel)
{
}

void FreeFlyCameraController::takeOver(const Camera& camera) {
    angles = anglesFromDirection(camera.getForward());
}

/*
* Turn first, then move along the new view direction, so movement always matches what is on screen.
*/
void FreeFlyCameraController::update(Camera& camera, const Input& input, float frameSeconds) {
    if (!input.isMouseCaptured()) {
        return;
    }

    applyMouseLook(angles, input.getMouseDelta(), radiansPerPixel);
    camera.setForward(directionFromAngles(angles));

    const Vec2 axes = getMoveAxes(input);
    const Vec3 worldUp{0.0f, 1.0f, 0.0f};

    Vec3 movement = camera.getForward() * axes.y + camera.getRight() * axes.x;

    if (input.isKeyHeld(Key::Space)) {
        movement = movement + worldUp;
    }

    if (input.isKeyHeld(Key::LeftCtrl) || input.isKeyHeld(Key::RightCtrl)) {
        movement = movement - worldUp;
    }

    // Normalizing keeps diagonal movement from being faster.
    if (movement.lengthSquared() > 0.0f) {
        camera.setPosition(camera.getPosition() + movement.normalized() * (moveSpeed * getSpeedMultiplier(input) * frameSeconds));
    }
}

const ViewAngles& FreeFlyCameraController::getAngles() const {
    return angles;
}
