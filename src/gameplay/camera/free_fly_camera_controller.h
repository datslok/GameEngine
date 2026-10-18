#pragma once

#include "gameplay/camera/look_controls.h"
#include "input/input.h"
#include "scene/camera.h"

struct FreeFlyCameraSettings {
    LookSettings look;
    float moveSpeed = 3.0f; // units per second
};

/*
* A debug or spectator camera: the mouse turns it, WASD moves along the view direction, Space and Ctrl move straight up and down,
* and holding Shift flies faster.
* Pauses while the mouse is not captured (after Escape).
*/
class FreeFlyCameraController {
public:
    explicit FreeFlyCameraController(const FreeFlyCameraSettings& settings = {});

    // Continue from wherever the camera currently looks.
    void takeOver(const Camera& camera);

    void update(Camera& camera, const Input& input, float frameSeconds);

    const ViewAngles& getAngles() const;

    // Read every frame, so changes take effect at once.
    FreeFlyCameraSettings& getSettings();

private:
    FreeFlyCameraSettings settings;
    ViewAngles angles;
};
