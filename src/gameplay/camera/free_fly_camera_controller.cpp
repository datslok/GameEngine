#include "gameplay/camera/free_fly_camera_controller.h"

FreeFlyCameraController::FreeFlyCameraController(const FreeFlyCameraSettings& settings):
    settings(settings)
{
}

void FreeFlyCameraController::takeOver(const Camera& camera) {
    angles = anglesFromDirection(camera.getForward());
}

/*
* Turn first, then move along the new view direction, so movement always matches what is on screen.
*/
void FreeFlyCameraController::update(Camera& camera, const Input& input, float frameSeconds) {
    if (!input.isMouseCaptured()) {
        return;
    }

    turnCamera(camera, angles, input, settings.look);

    const Vec2 axes = getMoveAxes(input);
    const Vec3 worldUp{0.0f, 1.0f, 0.0f};

    Vec3 movement = camera.getForward() * axes.y + camera.getRight() * axes.x;

    if (input.isKeyHeld(Key::Space)) {
        movement = movement + worldUp;
    }

    if (input.isKeyHeld(Key::LeftCtrl) || input.isKeyHeld(Key::RightCtrl)) {
        movement = movement - worldUp;
    }

    moveCamera(camera, movement, settings.moveSpeed, input, settings.look, frameSeconds);
}

const ViewAngles& FreeFlyCameraController::getAngles() const {
    return angles;
}

FreeFlyCameraSettings& FreeFlyCameraController::getSettings() {
    return settings;
}

void visit(FreeFlyCameraSettings& settings, SettingsVisitor& visitor) {
    visitor.group("Look", [&] {
        visit(settings.look, visitor);
    });
    visitor.field("Move speed", settings.moveSpeed, {.min = 0.0f, .sliderMax = 50.0f, .unit = "m/s"});
}
