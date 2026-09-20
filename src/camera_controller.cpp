#include "camera_controller.h"

#include <SDL3/SDL.h>

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
void CameraController::update(Camera& camera, float deltaTime) const {
    const bool* keys = SDL_GetKeyboardState(nullptr);

    Vec3 movement{};

    if (keys[SDL_SCANCODE_W]) {
        movement = movement + camera.getForward();
    }

    if (keys[SDL_SCANCODE_S]) {
        movement = movement - camera.getForward();
    }

    if (keys[SDL_SCANCODE_A]) {
        movement = movement - camera.getRight();
    }

    if (keys[SDL_SCANCODE_D]) {
        movement = movement + camera.getRight();
    }

    // Ctrl moves down.
    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) {
        movement = movement - camera.getUp();
    }

    // Space moves up.
    if (keys[SDL_SCANCODE_SPACE]) {
        movement = movement + camera.getUp();
    }

    if (movement.lengthSquared() > 0.0f) {
        camera.move(
            movement.normalized() * (moveSpeed * deltaTime)
        );
    }
}

void CameraController::look(Camera& camera, float mouseDeltaX, float mouseDeltaY) const {
    camera.rotate(mouseDeltaX * mouseSensitivity, -mouseDeltaY * mouseSensitivity);
}