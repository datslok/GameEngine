#pragma once

#include "gameplay/camera/look_controls.h"
#include "input/input.h"
#include "scene/camera.h"

/*
* A first-person camera: the mouse turns it, and WASD walks on the ground plane, so looking up or down never changes height.
* Holding Shift runs at double speed. Space jumps; gravity brings the eyes back to the height they had when the controller
* took over, which stands in for the ground (there is no collision yet).
* For now it moves the camera itself. Once there is a character controller (phase 5) it will follow a body with collision instead.
* Pauses while the mouse is not captured (after Escape).
*/
class FirstPersonCameraController {
public:
    // jumpHeight is how far above eye height a jump reaches; gravity is in units per second squared.
    FirstPersonCameraController(float moveSpeed, float radiansPerPixel, float jumpHeight = 1.2f, float gravity = 20.0f);

    // Continue from wherever the camera currently looks, standing on the ground at its current height.
    void takeOver(const Camera& camera);

    bool isAirborne() const;

    void update(Camera& camera, const Input& input, float frameSeconds);

    const ViewAngles& getAngles() const;

private:
    float moveSpeed;
    float radiansPerPixel;
    ViewAngles angles;

    float gravity;
    float jumpSpeed;           // the upward speed that reaches jumpHeight: sqrt(2 * gravity * jumpHeight)
    float eyeHeight = 0.0f;    // the camera's height when standing
    float verticalSpeed = 0.0f;
    bool airborne = false;

    void updateJump(Camera& camera, const Input& input, float frameSeconds);
};
