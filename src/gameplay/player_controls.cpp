#include "gameplay/player_controls.h"

#include "gameplay/camera/look_controls.h"
#include "gameplay/first_person_body.h"
#include "math/angles.h"
#include "physics/character_controller.h"
#include "scene/camera_ray.h"

#include <stdexcept>

namespace {
    const Vec3 worldUp{0.0f, 1.0f, 0.0f};

    // Replaced by the engine with the real frame's aspect ratio.
    constexpr float placeholderAspectRatio = 16.0f / 9.0f;
}

PlayerControls::PlayerControls(const PlayerControlsSetup& setup, ControlMode startMode, bool debugModeSwitching):
    setup(setup),
    camera(
        setup.freeCameraPosition,
        setup.freeCameraTarget,
        worldUp,
        radiansFromDegrees(setup.verticalFovDegrees),
        placeholderAspectRatio,
        setup.nearPlane,
        setup.farPlane
    ),
    // The camera controllers start with their default settings; change them through visitSettings.
    controlMode(startMode),
    enableDebugModeSwitching(debugModeSwitching)
{
}

void PlayerControls::spawnBody(World& world) {
    firstPersonBody = spawnFirstPersonBody(world, setup.firstPersonFeet);
}

void PlayerControls::setControlMode(World& world, ControlMode mode) {
    switch (mode) {
    case ControlMode::FirstPerson:
        // From the free camera, drop the body where the camera is (it falls onto whatever is below) and keep looking the
        // same way; otherwise look straight ahead from wherever the body stands.
        if (controlMode == ControlMode::FreeCamera) {
            const float eyeHeight = world.get<FirstPersonBody>(firstPersonBody).settings.standingEyeHeight;
            placeFirstPersonBody(world, firstPersonBody, camera.getPosition() - Vec3{0.0f, eyeHeight, 0.0f});
        } else {
            const Vec3 eye = firstPersonEyePosition(world, firstPersonBody, 1.0f);
            camera.setPose(eye, eye + Vec3{0.0f, 0.0f, -1.0f}, worldUp);
        }
        firstPersonCamera.takeOver(camera);
        break;

    case ControlMode::Moba:
        mobaCamera.centreOn(camera, setup.mobaLookPoint);
        break;

    case ControlMode::FreeCamera:
        camera.setPose(setup.freeCameraPosition, setup.freeCameraTarget, worldUp);
        freeFlyCamera.takeOver(camera);
        break;

    default:
        throw std::invalid_argument("Unknown control mode");
    }

    controlMode = mode;
}

bool PlayerControls::updateModeSwitching(World& world, const Input& input) {
    if (!enableDebugModeSwitching || !input.hasKeyboardFocus()) {
        return false;
    }

    if (input.wasKeyPressed(Key::F1)) {
        setControlMode(world, ControlMode::FirstPerson);
        return true;
    }

    if (input.wasKeyPressed(Key::F2)) {
        setControlMode(world, ControlMode::Moba);
        return true;
    }

    if (input.wasKeyPressed(Key::F3)) {
        setControlMode(world, ControlMode::FreeCamera);
        return true;
    }

    return false;
}

/*
* Each mode hands the camera to its own controller. The MOBA camera follows the game's unit only while locked, and
* follows where it is drawn, not where the latest tick put it, so the camera and the unit move together smoothly.
*/
void PlayerControls::updateCamera(const World& world, const Input& input, float frameSeconds, float alpha,
                                  const std::optional<Vec3>& mobaFollowTarget) {
    switch (controlMode) {
    case ControlMode::FirstPerson:
        firstPersonCamera.update(camera, input, frameSeconds);
        camera.setPosition(firstPersonEyePosition(world, firstPersonBody, alpha));
        break;

    case ControlMode::FreeCamera:
        freeFlyCamera.update(camera, input, frameSeconds);
        break;

    case ControlMode::Moba: {
        if (input.isCursorConfined() && input.wasKeyPressed(Key::Space)) {
            mobaCameraLocked = !mobaCameraLocked;
        }

        const std::optional<Vec3> followTarget = mobaCameraLocked ? mobaFollowTarget : std::nullopt;
        mobaCamera.update(camera, input, frameSeconds, followTarget);
        break;
    }
    }
}

/*
* Once a frame, the movement keys become commands for the body's next tick: WASD a wished velocity turned by where the
* camera faces, Shift sprints, Ctrl crouches, and a fresh press of Space jumps (holding it does not jump again on
* landing). Outside first person, or with the mouse released, the body is told to stand still.
*/
bool PlayerControls::updateFirstPersonCommands(World& world, const Input& input) {
    CharacterController& controller = world.get<CharacterController>(firstPersonBody);
    FirstPersonBody& body = world.get<FirstPersonBody>(firstPersonBody);

    if (controlMode != ControlMode::FirstPerson || !input.isMouseCaptured()) {
        controller.wishVelocity = Vec3{};
        controller.wantsJump = false;
        body.wantsCrouch = false;
        return false;
    }

    body.wantsCrouch = input.isKeyHeld(Key::LeftCtrl) || input.isKeyHeld(Key::RightCtrl);
    const bool sprinting = input.isKeyHeld(Key::LeftShift) || input.isKeyHeld(Key::RightShift);
    controller.wishVelocity = firstPersonWishVelocity(getMoveAxes(input), firstPersonCamera.getAngles().yaw, body.crouching,
                                                      sprinting, body.settings);
    if (input.wasKeyPressed(Key::Space)) {
        controller.wantsJump = true;
    }
    return true;
}

void PlayerControls::poseFreeCamera(World& world, const Vec3& position, const Vec3& target) {
    setControlMode(world, ControlMode::FreeCamera);
    camera.setPose(position, target, worldUp);
    freeFlyCamera.takeOver(camera);
}

Camera& PlayerControls::getCamera() {
    return camera;
}

const Camera& PlayerControls::getCamera() const {
    return camera;
}

ControlMode PlayerControls::getMode() const {
    return controlMode;
}

Entity PlayerControls::getFirstPersonBody() const {
    return firstPersonBody;
}

bool PlayerControls::wantsMouseLook() const {
    return controlMode != ControlMode::Moba;
}

/*
* The MOBA camera floats about 16 units from the ground it shows, beyond the reach of a torch-sized light, so lights are
* ranked by their brightness at the ground point in the middle of the screen. The other modes see from among the lights.
*/
Vec3 PlayerControls::getLightFocus() const {
    if (controlMode == ControlMode::Moba) {
        return mobaCamera.getLookPoint(camera);
    }

    return camera.getPosition();
}

// Read every frame, so changes show at once.
void PlayerControls::visitSettings(SettingsVisitor& visitor) {
    visitor.group("First person camera", [&] {
        visit(firstPersonCamera.getSettings(), visitor);
    });
    visitor.group("MOBA camera", [&] {
        visit(mobaCamera.getSettings(), visitor);
    });
    visitor.group("Free camera", [&] {
        visit(freeFlyCamera.getSettings(), visitor);
    });
}

std::optional<Ray> PlayerControls::rayThroughPixel(const Input& input, const Vec2& pixel) const {
    const Vec2 size = input.getWindowSize();

    if (size.x <= 0.0f || size.y <= 0.0f ||
        pixel.x < 0.0f || pixel.y < 0.0f ||
        pixel.x >= size.x || pixel.y >= size.y) {
        return std::nullopt;
    }

    return makeCameraRay(camera, pixel.x / size.x, pixel.y / size.y, size.x / size.y);
}

std::optional<Ray> PlayerControls::pointerRay(const Input& input) const {
    if (wantsMouseLook()) {
        return rayThroughPixel(input, input.getWindowSize() * 0.5f);
    }
    if (!input.isCursorInWindow()) {
        return std::nullopt;
    }
    return rayThroughPixel(input, input.getCursorPosition());
}
