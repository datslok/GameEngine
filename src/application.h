#pragma once

#include "camera.h"
#include "camera_controller.h"
#include "display.h"
#include "mesh.h"
#include "pixelbuffer.h"
#include "renderer.h"
#include "mesh_instance.h"
#include "scene.h"

#include <vector>
#include <cstdint>

class Application {
public:
    Application(int width, int height);

    void run();

private:
    void update(float deltaTime);
    void render();
    void createScene();

    // Members are constructed in this order.
    PixelBuffer buffer;
    Display display;
    Renderer renderer;

    Camera camera;
    CameraController cameraController;
    Scene scene;

    double elapsedSeconds = 0.0;
    std::uint64_t targetFPS = 240;
};