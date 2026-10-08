#pragma once

#include "gameplay/camera/look_controls.h"
#include "input/input.h"
#include "scene/camera.h"

/*
* A first-person camera: the mouse turns it, and WASD walks on the ground plane, so looking up or down never changes height.
* For now it moves the camera itself. Once there is a character controller (phase 5) it will follow a body with collision instead.
* Pauses while the mouse is not captured (after Escape).
*/
class FirstPersonCameraController {
public:
    FirstPersonCameraController(float moveSpeed, float radiansPerPixel);

    // Continue from wherever the camera currently looks.
    void takeOver(const Camera& camera);

    void update(Camera& camera, const Input& input, float frameSeconds);

    const ViewAngles& getAngles() const;

private:
    float moveSpeed;
    float radiansPerPixel;
    ViewAngles angles;
};
