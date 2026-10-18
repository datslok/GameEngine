#include "gameplay/camera/first_person_camera_controller.h"

FirstPersonCameraController::FirstPersonCameraController(const FirstPersonCameraSettings& settings):
    settings(settings)
{
}

void FirstPersonCameraController::takeOver(const Camera& camera) {
    angles = anglesFromDirection(camera.getForward());
}

/*
* Only turning: walking belongs to the body, and the frame time is not needed to turn by mouse motion.
*/
void FirstPersonCameraController::update(Camera& camera, const Input& input, float) {
    if (!input.isMouseCaptured()) {
        return;
    }

    turnCamera(camera, angles, input, settings.look);
}

const ViewAngles& FirstPersonCameraController::getAngles() const {
    return angles;
}

FirstPersonCameraSettings& FirstPersonCameraController::getSettings() {
    return settings;
}
