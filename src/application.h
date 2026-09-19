#pragma once

#include "camera.h"
#include "camera_controller.h"
#include "display.h"
#include "mesh.h"
#include "pixelbuffer.h"
#include "renderer.h"

#include <cstdint>

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

    // Members are initialised in this order to match their declaration order.
    PixelBuffer buffer;
    Display display;
    Renderer renderer;

    Camera camera;
    CameraController cameraController;
    Mesh cube;

    double elapsedSeconds = 0.0;
    std::uint64_t targetFPS = 240;
};