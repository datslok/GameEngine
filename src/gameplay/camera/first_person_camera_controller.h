#pragma once

#include "core/settings_visitor.h"
#include "gameplay/camera/look_controls.h"
#include "input/input.h"
#include "scene/camera.h"

struct FirstPersonCameraSettings {
    LookSettings look;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(FirstPersonCameraSettings& settings, SettingsVisitor& visitor);

/*
* A first-person camera: the mouse turns it, every rendered frame, so aim follows the mouse at full frame rate. It does
* not move: the game puts it at the eyes of the body that walks (gameplay/first_person_body), whose movement keys are
* turned by this camera's yaw. Pauses while the mouse is not captured (after Escape).
*/
class FirstPersonCameraController {
public:
    explicit FirstPersonCameraController(const FirstPersonCameraSettings& settings = {});

    // Continue from wherever the camera currently looks.
    void takeOver(const Camera& camera);

    void update(Camera& camera, const Input& input, float frameSeconds);

    const ViewAngles& getAngles() const;

    // Read every frame, so changes take effect at once.
    FirstPersonCameraSettings& getSettings();

private:
    FirstPersonCameraSettings settings;
    ViewAngles angles;
};
