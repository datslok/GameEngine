#pragma once

#include "camera.h"

/*
* Controls the camera movement using keyboard input and a configurable movement speed to provide consistent movement over time.
*/
class CameraController {
    public:
        explicit CameraController(float moveSpeed);

        void update(Camera& camera, float deltaTime) const;

    private:
        float moveSpeed;
};