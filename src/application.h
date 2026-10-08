#pragma once

#include "camera.h"
#include "camera_controller.h"
#include "gpu_display.h"
#include "gpu_mesh.h"
#include "mesh.h"
#include "scene.h"
#include "character.h"
#include "control_mode.h"
#include "input.h"
#include "fixed_timestep.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <optional>
#include <cstddef>

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

    // Constructed first and destroyed last.
    // The GPU device must outlive all uploaded meshes.
    GpuDisplay display;

    // Refreshed once per frame by display.processEvents().
    Input input;

    Camera camera;
    CameraController cameraController;
    Scene scene;

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

    std::uint64_t targetFPS = 240;

    std::optional<Character> playerCharacter;
    void updateCameraControls(float deltaTime);

    ControlMode controlMode = ControlMode::FreeCamera;
    bool enableDebugModeSwitching = true;
    void updateMobaCamera(float deltaTime);

    bool mobaCameraLocked = false;
    float mobaPanSpeed = 9.0f;

    // Relative to the character's visual centre.
    Vec3 mobaCameraOffset{0.0f, 12.0f, 10.0f};

    void updatePlayerCommands();
    void followPlayerWithCamera(float alpha);
    std::size_t destinationMarkerIndex = 0;
};