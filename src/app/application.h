#pragma once

#include "scene/camera.h"
#include "gameplay/camera_controller.h"
#include "platform/window.h"
#include "render/gpu/gpu_renderer.h"
#include "render/gpu/gpu_mesh.h"
#include "scene/mesh.h"
#include "ecs/world.h"
#include "gameplay/character.h"
#include "gameplay/control_mode.h"
#include "input/input.h"
#include "core/fixed_timestep.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

/*
* Coordinates the main engine components to manage input, updates, rendering, and the applications main execution loop.
*/
class Application {
public:
    Application(int width, int height);

    void run();
    void setControlMode(ControlMode mode);
    void setDebugModeSwitching(bool enabled);

private:
    // Advance the game state by exactly one fixed tick.
    void simulate(float tickSeconds);
    void render(float alpha);
    void createScene();
    void uploadSceneMeshes();

    // Members are destroyed in reverse order: the window outlives the renderer that draws into it,
    // and the renderer's GPU device outlives every uploaded mesh below.
    Window window;
    GpuRenderer renderer;

    // Refreshed once per frame by window.processEvents().
    Input input;

    Camera camera;
    CameraController cameraController;
    World world;

    // Each shared CPU mesh maps to one uploaded GPU mesh.
    std::unordered_map<
        std::shared_ptr<const Mesh>,
        std::unique_ptr<GpuMesh>
    > gpuMeshes;

    // The simulation always advances in steps of exactly 1/120 s.
    // Frames longer than maxFrameSeconds are clamped, so a hitch slows the game down instead of piling up catch-up ticks.
    static constexpr double simulationTicksPerSecond = 120.0;
    static constexpr double maxFrameSeconds = 0.25;
    FixedTimestep timestep{simulationTicksPerSecond, maxFrameSeconds};

    // Counting ticks instead of adding up seconds keeps simulation time exact.
    std::uint64_t simulationTicks = 0;

    // With mailbox presentation the loop is no longer held back by vsync, so this cap sets the real frame rate.
    std::uint64_t targetFPS = 240;

    // Entity{} means there is no player.
    Entity player;
    void updateCameraControls(float deltaTime);

    ControlMode controlMode = ControlMode::FreeCamera;
    bool enableDebugModeSwitching = true;
    void updateMobaCamera(float deltaTime);

    bool mobaCameraLocked = false;
    float mobaPanSpeed = 9.0f;

    // Relative to the character's visual centre.
    Vec3 mobaCameraOffset{0.0f, 12.0f, 10.0f};

    void updatePlayerCommands();

    // True while a right-click that started in MOBA mode is still held, so the player keeps following the cursor.
    bool groundSteeringActive = false;

    void followPlayerWithCamera(float alpha);
    Entity destinationMarker;
};