#pragma once

#include "ecs/world.h"
#include "engine/game.h"
#include "gameplay/camera/first_person_camera_controller.h"
#include "gameplay/camera/free_fly_camera_controller.h"
#include "gameplay/camera/moba_camera_controller.h"
#include "games/demo/control_mode.h"
#include "input/input.h"
#include "scene/camera.h"

/*
* The demo scene: spinning objects, a ground plane and a duck you can steer by clicking.
* F1/F2/F3 switch between first person, MOBA and free camera when debug mode switching is on.
*/
class DemoGame final : public Game {
public:
    explicit DemoGame(ControlMode startMode = ControlMode::FreeCamera, bool debugModeSwitching = true);

    void onInit(World& world, AssetManager& assets) override;
    void onInput(World& world, const Input& input) override;
    void onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) override;
    void onUpdate(World& world, const Input& input, float frameSeconds, float alpha) override;
    Camera& getCamera() override;
    bool wantsMouseLook() const override;

    // For tests and debugging.
    Entity getPlayer() const;
    Entity getDestinationMarker() const;

private:
    void createScene(World& world, AssetManager& assets);
    void setControlMode(World& world, ControlMode mode);

    // Turn right-clicks into move commands for the player.
    void updatePlayerCommands(World& world, const Input& input);

    // Returns true when the mode changed, so this frame's mouse motion is not applied to the new camera pose.
    bool updateModeSwitching(World& world, const Input& input);

    // Run the controller for the current mode.
    void updateCamera(const World& world, const Input& input, float frameSeconds, float alpha);

    Camera camera;

    // A real game would use just one of these. The demo keeps all three and switches with F1-F3.
    FirstPersonCameraController firstPersonCamera;
    MobaCameraController mobaCamera;
    FreeFlyCameraController freeFlyCamera;

    ControlMode controlMode;
    bool enableDebugModeSwitching;

    // Entity{} means "none" until onInit creates them.
    Entity player;
    Entity destinationMarker;

    // True while a right-click that started in MOBA mode is still held, so the player keeps following the cursor.
    bool groundSteeringActive = false;

    // Space toggles following the player in MOBA mode.
    bool mobaCameraLocked = false;
};
