#include "camera.h"
#include "camera_controller.h"
#include "display.h"
#include "mat4.h"
#include "mesh.h"
#include "pixelbuffer.h"
#include "renderer.h"
#include "vec3.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <exception>
#include <iostream>
#include <numbers>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    try {
        PixelBuffer buffer{1280, 720};
        Display display{"My Engine", 1280, 720};
        Renderer renderer{buffer};

        Uint64 targetFPS = 240;

        const float aspectRatio =
            static_cast<float>(buffer.getWidth()) /
            static_cast<float>(buffer.getHeight());

        Camera camera{
            Vec3{2.0f, 1.0f, 0.0f},
            Vec3{0.0f, 0.0f, -5.0f},
            Vec3{0.0f, 1.0f, 0.0f},
            std::numbers::pi_v<float> / 2.0f,
            aspectRatio,
            0.1f,
            100.0f
        };

        CameraController cameraController{3.0f}; // camera speed
        const Mesh cube = Mesh::cube();

        const Uint64 animationStart = SDL_GetTicksNS();
        Uint64 previousFrameStart = animationStart;

        while (true) {
            const Uint64 frameStart = SDL_GetTicksNS();

            if (!display.processEvents()) {
                break;
            }

            const float deltaTime = static_cast<float>(
                static_cast<double>(frameStart - previousFrameStart) /
                1'000'000'000.0
            );

            previousFrameStart = frameStart;
            cameraController.update(camera, deltaTime);

            const float elapsedSeconds = static_cast<float>(
                static_cast<double>(frameStart - animationStart) /
                1'000'000'000.0
            );

            const float angle = elapsedSeconds * 2.0f; // rotation speed

            const Mat4 model =
                Mat4::translation(0.0f, 0.0f, -5.0f) *
                Mat4::rotationY(angle) *
                Mat4::rotationX(0.3f);

            renderer.clear(Pixel{0, 0, 0});
            renderer.drawMesh(cube, model, camera, Pixel{80, 120, 220}, true);
            display.present(buffer);

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
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}