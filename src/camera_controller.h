#pragma once

#include "camera.h"

// Controls camera movement and mouse-look sensitivity.
class CameraController {
public:
    explicit CameraController(
        float moveSpeed,
        float mouseSensitivity = 0.002f
    );

    void update(Camera& camera, float deltaTime) const;

    void look(
        Camera& camera,
        float mouseDeltaX,
        float mouseDeltaY
    ) const;

private:
    float moveSpeed;

    // Radians of rotation per unit of mouse movement.
    float mouseSensitivity;
};