#pragma once

#include "camera.h"
#include "camera_controller.h"
#include "gpu_display.h"
#include "gpu_mesh.h"
#include "mesh.h"
#include "scene.h"
#include "model_instance.h"
#include "control_mode.h"
#include "move_to_controller.h"

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
    void update(float deltaTime);
    void render();
    void createScene();
    void uploadSceneMeshes();

    // Constructed first and destroyed last.
    // The GPU device must outlive all uploaded meshes.
    GpuDisplay display;

    Camera camera;
    CameraController cameraController;
    Scene scene;

    // Each shared CPU mesh maps to one uploaded GPU mesh.
    std::unordered_map<
        std::shared_ptr<const Mesh>,
        std::unique_ptr<GpuMesh>
    > gpuMeshes;

    double elapsedSeconds = 0.0;
    std::uint64_t targetFPS = 240;

    std::shared_ptr<ModelInstance> duck;
    void updateCameraControls(float deltaTime);

    ControlMode controlMode = ControlMode::FreeCamera;
    bool enableDebugModeSwitching = true;
    void updateMobaCamera(float deltaTime);

    bool mobaCameraLocked = false;
    float mobaPanSpeed = 9.0f;

    // Relative to the duck's position.
    Vec3 mobaCameraOffset{0.0f, 12.0f, 10.0f};
    MoveToController duckMovement{3.0f};

    void updateDuckMovement(float deltaTime);
    void followDuckWithCamera();
    std::optional<float> duckTargetYaw;
    std::size_t destinationMarkerIndex = 0;
};