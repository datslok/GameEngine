#pragma once

#include "camera.h"

class CameraController {
    public:
        explicit CameraController(float moveSpeed);

        void update(Camera& camera, float deltaTime) const;

    private:
        float moveSpeed;
};