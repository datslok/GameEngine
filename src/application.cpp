#include "application.h"

#include "mat4.h"
#include "vec3.h"

#include <SDL3/SDL.h>

#include <numbers>

Application::Application(int width, int height)
    : buffer(width, height),
      display("My Engine", width, height),
      renderer(buffer),
      camera(
          Vec3{2.0f, 1.0f, 0.0f},
          Vec3{0.0f, 0.0f, -5.0f},
          Vec3{0.0f, 1.0f, 0.0f},
          std::numbers::pi_v<float> / 2.0f,
          static_cast<float>(buffer.getWidth()) /
              static_cast<float>(buffer.getHeight()),
          0.1f,
          100.0f
      ),
      cameraController(3.0f),
      cube(Mesh::cube()) {
}

void Application::run() {
    Uint64 previousFrameStart = SDL_GetTicksNS();

    while (true) {
        const Uint64 frameStart = SDL_GetTicksNS();

        if (!display.processEvents()) {
            break;
        }

        const double deltaSeconds =
            static_cast<double>(frameStart - previousFrameStart) /
            1'000'000'000.0;

        previousFrameStart = frameStart;
        elapsedSeconds += deltaSeconds;

        update(static_cast<float>(deltaSeconds));
        render();

        // 0 means unlimited.
        if (targetFPS > 0) {
            const Uint64 frameDuration =
                1'000'000'000ULL / targetFPS;

            const Uint64 elapsed =
                SDL_GetTicksNS() - frameStart;

            if (elapsed < frameDuration) {
                SDL_DelayNS(frameDuration - elapsed);
            }
        }
    }
}

void Application::update(float deltaTime) {
    cameraController.update(camera, deltaTime);
}

void Application::render(){
    const float angle =
        static_cast<float>(elapsedSeconds * 0.2f); // rotation speed

    const Mat4 model =
        Mat4::translation(0.0f, 0.0f, -5.0f) * Mat4::rotationY(angle) * Mat4::rotationX(0.3f);

    renderer.clear(Pixel{0, 0, 0});

    renderer.drawMesh(cube, model, camera, Pixel{80, 120, 220}, false);

    display.present(buffer);
}