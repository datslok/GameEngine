#pragma once

#include "gameplay/camera/look_controls.h"
#include "input/input.h"
#include "scene/camera.h"

struct FirstPersonCameraSettings {
    LookSettings look;
    float moveSpeed = 3.0f;  // units per second
    float jumpHeight = 1.2f; // how far above eye height a jump reaches
    float gravity = 20.0f;   // units per second squared
};

/*
* A first-person camera: the mouse turns it, and WASD walks on the ground plane, so looking up or down never changes height.
* Holding Shift runs. Space jumps; gravity brings the eyes back to the height they had when the controller
* took over, which stands in for the ground (there is no collision yet).
* For now it moves the camera itself. Once there is a character controller (phase 5) it will follow a body with collision instead.
* Pauses while the mouse is not captured (after Escape).
*/
class FirstPersonCameraController {
public:
    explicit FirstPersonCameraController(const FirstPersonCameraSettings& settings = {});

    // Continue from wherever the camera currently looks, standing on the ground at its current height.
    void takeOver(const Camera& camera);

    bool isAirborne() const;

    void update(Camera& camera, const Input& input, float frameSeconds);

    const ViewAngles& getAngles() const;

    // Read every frame, so changes take effect at once (a new jump height from the next jump).
    FirstPersonCameraSettings& getSettings();

private:
    FirstPersonCameraSettings settings;
    ViewAngles angles;

    float eyeHeight = 0.0f;    // the camera's height when standing
    float verticalSpeed = 0.0f;
    bool airborne = false;

    void updateJump(Camera& camera, const Input& input, float frameSeconds);
};
