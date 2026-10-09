#pragma once

#include "gameplay/camera/look_controls.h"
#include "input/input.h"
#include "scene/camera.h"

/*
* A debug or spectator camera: the mouse turns it, WASD moves along the view direction, Space and Ctrl move straight up and down,
* and holding Shift flies at double speed.
* Pauses while the mouse is not captured (after Escape).
*/
class FreeFlyCameraController {
public:
    FreeFlyCameraController(float moveSpeed, float radiansPerPixel);

    // Continue from wherever the camera currently looks.
    void takeOver(const Camera& camera);

    void update(Camera& camera, const Input& input, float frameSeconds);

    const ViewAngles& getAngles() const;

private:
    float moveSpeed;
    float radiansPerPixel;
    ViewAngles angles;
};
