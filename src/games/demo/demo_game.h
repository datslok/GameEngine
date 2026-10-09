#pragma once

#include "ecs/world.h"
#include "engine/game.h"
#include "gameplay/camera/first_person_camera_controller.h"
#include "gameplay/camera/free_fly_camera_controller.h"
#include "gameplay/camera/moba_camera_controller.h"
#include "games/demo/control_mode.h"
#include "input/input.h"
#include "scene/camera.h"
#include "scene/light.h"

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
    void onDebugDraw(const World& world, DebugDraw& debug, float alpha) override;
    bool wantsBoundingBoxes() const override;

    // For tests and debugging.
    Entity getPlayer() const;
    Entity getDestinationMarker() const;
    Entity getFlashlight() const;
    Entity getMoon() const;

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

    // F toggles it. Held at the camera, or by the duck in MOBA mode.
    Entity flashlight;
    // A large source radius keeps it gentle up close; the higher intensity carries it further.
    SpotLight flashlightBeam{
        .colour = Vec3{1.0f, 0.89f, 0.69f},
        .intensity = 12.0f,
        .range = 50.0f,
        .sourceRadius = 4.5f
    };
    void toggleFlashlight(World& world);
    void updateFlashlight(World& world, float alpha);

    // A glowing sphere with the moonlight inside it, at a fixed place you can fly to in free camera mode.
    Entity moon;

    // True while a right-click that started in MOBA mode is still held, so the player keeps following the cursor.
    bool groundSteeringActive = false;

    // F4: show bounding boxes, light markers and the walk target.
    bool debugViewEnabled = false;

    // Space toggles following the player in MOBA mode.
    bool mobaCameraLocked = false;
};
