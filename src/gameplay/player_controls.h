#pragma once

#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "gameplay/camera/first_person_camera_controller.h"
#include "gameplay/camera/free_fly_camera_controller.h"
#include "gameplay/camera/moba_camera_controller.h"
#include "gameplay/control_mode.h"
#include "input/input.h"
#include "math/vec3.h"
#include "scene/camera.h"

#include <optional>

/*
* Where a game's views start: the free camera's pose (F3), the ground point the MOBA camera first looks at (F2), where
* the first-person body stands at the start (F1), and the lens all three share.
*/
struct PlayerControlsSetup {
    Vec3 freeCameraPosition{2.0f, 1.0f, 0.0f};
    Vec3 freeCameraTarget{0.0f, 0.0f, -5.0f};
    Vec3 mobaLookPoint{0.0f, 0.0f, -6.0f};
    Vec3 firstPersonFeet{0.0f, 0.0f, 0.0f};
    float verticalFovDegrees = 70.0f;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
};

/*
* The player's ways of looking at a game, shared by every game that wants them: one camera, its three controllers
* (first person, MOBA, free camera), the first-person body that walks the level, and switching between them with F1-F3.
* A game holds one as a member and forwards to it (composition: a game that wants other controls simply holds none);
* what a mode means for the game's own things (a MOBA unit's orders, say) stays in the game.
*/
class PlayerControls {
public:
    PlayerControls(const PlayerControlsSetup& setup, ControlMode startMode, bool debugModeSwitching);

    // Create the first-person body at the setup's feet. Call it once from onInit, where the game wants it among its
    // entities, before the first setControlMode.
    void spawnBody(World& world);

    /*
    * Switch to a mode from its predictable start: first person from the free camera drops the body where the camera is
    * (it falls onto whatever is below) and keeps the view, otherwise it looks straight ahead (-z) from the body's eyes;
    * MOBA centres on the setup's look point; the free camera goes to the setup's pose.
    */
    void setControlMode(World& world, ControlMode mode);

    // F1-F3 while debug switching is on and the window has the keyboard. Returns true when the mode changed, so this
    // frame's mouse motion is not applied to the new camera pose.
    bool updateModeSwitching(World& world, const Input& input);

    // Run the current mode's controller. In MOBA mode Space locks the camera to mobaFollowTarget (where the game's unit
    // is drawn), when the game has one.
    void updateCamera(const World& world, const Input& input, float frameSeconds, float alpha,
                      const std::optional<Vec3>& mobaFollowTarget = std::nullopt);

    // The movement keys, Space and Ctrl as commands for the body, in first person with the mouse captured; otherwise the
    // body is told to stand still. Returns whether first-person input counted this frame (for the game's own keys).
    bool updateFirstPersonCommands(World& world, const Input& input);

    // Into the free camera at a given pose: what golden views and tests start from.
    void poseFreeCamera(World& world, const Vec3& position, const Vec3& target);

    Camera& getCamera();
    const Camera& getCamera() const;
    ControlMode getMode() const;
    Entity getFirstPersonBody() const;

    // A hidden, captured mouse in first person and free camera; a visible cursor in MOBA mode.
    bool wantsMouseLook() const;

    // Where lights are ranked: the ground point in the middle of the MOBA view (its camera floats beyond a torch's
    // reach), otherwise the camera itself.
    Vec3 getLightFocus() const;

    // The three cameras' settings, one group each.
    void visitSettings(SettingsVisitor& visitor);

private:
    PlayerControlsSetup setup;
    Camera camera;

    FirstPersonCameraController firstPersonCamera;
    MobaCameraController mobaCamera;
    FreeFlyCameraController freeFlyCamera;

    ControlMode controlMode;
    bool enableDebugModeSwitching;

    // Space toggles following the game's unit in MOBA mode.
    bool mobaCameraLocked = false;

    Entity firstPersonBody;
};
