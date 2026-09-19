#pragma once

#include "camera.h"
#include "camera_controller.h"
#include "gpu_display.h"
#include "gpu_mesh.h"
#include "mesh.h"
#include "scene.h"

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
};