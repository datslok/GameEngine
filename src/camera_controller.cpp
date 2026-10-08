#include "camera_controller.h"

/*
* Set the movement speed in order to consistently control camera movement.
*/
CameraController::CameraController(float moveSpeed, float mouseSensitivity):
    moveSpeed(moveSpeed),
    mouseSensitivity(mouseSensitivity)
{

}

/*
* Process keyboard input to move the camera in the direction of the pressed keys, scaled by movement speed and delta time for consistent movement across frame rates.
*/
void CameraController::update(
    Camera& camera,
    const Input& input,
    float deltaTime,
    ControlMode mode
) const {
    if (mode == ControlMode::Moba) {
        return;
    }

    Vec3 forward;
    Vec3 right;

    switch (mode) {
    case ControlMode::FirstPerson: {
        // Looking up or down must not change movement height.
        const Vec3 direction = camera.getForward();

        forward = Vec3{
            direction.x,
            0.0f,
            direction.z
        }.normalized();

        right = forward.cross(Vec3{0.0f, 1.0f, 0.0f}).normalized();
        break;
    }

    case ControlMode::Moba:
        // Fixed world directions for the overhead camera.
        forward = Vec3{0.0f, 0.0f, -1.0f};
        right = Vec3{1.0f, 0.0f, 0.0f};
        break;

    case ControlMode::FreeCamera:
        forward = camera.getForward();
        right = camera.getRight();
        break;
    }

    Vec3 movement{};

    if (input.isKeyHeld(Key::W)) {
        movement = movement + forward;
    }

    if (input.isKeyHeld(Key::S)) {
        movement = movement - forward;
    }

    if (input.isKeyHeld(Key::A)) {
        movement = movement - right;
    }

    if (input.isKeyHeld(Key::D)) {
        movement = movement + right;
    }

    if (mode == ControlMode::FreeCamera) {
        if (input.isKeyHeld(Key::Space)) {
            movement = movement + camera.getUp();
        }

        if (input.isKeyHeld(Key::LeftCtrl) ||
            input.isKeyHeld(Key::RightCtrl)) {
            movement = movement - camera.getUp();
        }
    }

    if (movement.lengthSquared() > 0.0f) {
        // Prevent diagonal movement from being faster.
        const float speed =
            mode == ControlMode::Moba ? moveSpeed * 3.0f : moveSpeed;

        camera.move(
            movement.normalized() * (speed * deltaTime)
        );
    }
}

void CameraController::look(Camera& camera, float mouseDeltaX, float mouseDeltaY) const {
    camera.rotate(mouseDeltaX * mouseSensitivity, -mouseDeltaY * mouseSensitivity);
}