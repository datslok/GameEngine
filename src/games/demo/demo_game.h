#pragma once

#include "ecs/world.h"
#include "engine/game.h"
#include "gameplay/camera_controller.h"
#include "gameplay/control_mode.h"
#include "input/input.h"
#include "scene/camera.h"

/*
* The demo scene: spinning objects, a ground plane and a duck you can steer by clicking.
* F1/F2/F3 switch between first person, MOBA and free camera when debug mode switching is on.
*/
class DemoGame final : public Game {
public:
    explicit DemoGame(ControlMode startMode = ControlMode::FreeCamera, bool debugModeSwitching = true);

    void onInit(World& world) override;
    void onInput(World& world, const Input& input) override;
    void onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) override;
    void onUpdate(World& world, const Input& input, float frameSeconds, float alpha) override;
    Camera& getCamera() override;
    bool wantsMouseLook() const override;

    // For tests and debugging.
    Entity getPlayer() const;
    Entity getDestinationMarker() const;

private:
    void createScene(World& world);
    void setControlMode(World& world, ControlMode mode);

    // Turn right-clicks into move commands for the player.
    void updatePlayerCommands(World& world, const Input& input);

    // Returns true when the mode changed, so this frame's mouse motion is not applied to the new camera pose.
    bool updateModeSwitching(World& world, const Input& input);

    void updateCameraControls(const Input& input, float frameSeconds);
    void updateMobaCamera(const Input& input, float frameSeconds);
    void followPlayerWithCamera(const World& world, float alpha);

    Camera camera;
    CameraController cameraController;

    ControlMode controlMode;
    bool enableDebugModeSwitching;

    // Entity{} means "none" until onInit creates them.
    Entity player;
    Entity destinationMarker;

    // True while a right-click that started in MOBA mode is still held, so the player keeps following the cursor.
    bool groundSteeringActive = false;

    bool mobaCameraLocked = false;
    float mobaPanSpeed = 9.0f;

    // Relative to the character's visual centre.
    Vec3 mobaCameraOffset{0.0f, 12.0f, 10.0f};
};
