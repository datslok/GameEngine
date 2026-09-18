#include "display.h"
#include "pixelbuffer.h"
#include "rasterizer.h"
#include "mat4.h"
#include "vec3.h"
#include "vec4.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <numbers>

#include "camera.h"
#include "camera_controller.h"
#include "clipper.h"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    try {
        PixelBuffer buffer{1280, 720};
        Display display{"My Engine", 1280, 720};

        Uint64 targetFPS = 240; // 0 means unlimited.

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

        CameraController cameraController{3.0f}; // Camera speed

        const std::array<Vec4, 8> vertices{
            Vec4{-1.0f, -1.0f, -1.0f, 1.0f},
            Vec4{ 1.0f, -1.0f, -1.0f, 1.0f},
            Vec4{ 1.0f,  1.0f, -1.0f, 1.0f},
            Vec4{-1.0f,  1.0f, -1.0f, 1.0f},

            Vec4{-1.0f, -1.0f,  1.0f, 1.0f},
            Vec4{ 1.0f, -1.0f,  1.0f, 1.0f},
            Vec4{ 1.0f,  1.0f,  1.0f, 1.0f},
            Vec4{-1.0f,  1.0f,  1.0f, 1.0f}
        };

        struct Edge {
            std::size_t start;
            std::size_t end;
        };

        const std::array<Edge, 12> edges{
            Edge{0, 1}, Edge{1, 2}, Edge{2, 3}, Edge{3, 0},
            Edge{4, 5}, Edge{5, 6}, Edge{6, 7}, Edge{7, 4},
            Edge{0, 4}, Edge{1, 5}, Edge{2, 6}, Edge{3, 7}
        };

        // Timing setup.
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

            // Keyboard movement.
            cameraController.update(camera, deltaTime);

            // Rotate the cube using elapsed time.
            const float elapsedSeconds = static_cast<float>(
                static_cast<double>(frameStart - animationStart) /
                1'000'000'000.0
            );

            const float angle = elapsedSeconds * 2.0f;

            const Mat4 model =
                Mat4::translation(0.0f, 0.0f, -5.0f) *
                Mat4::rotationY(angle) *
                Mat4::rotationX(0.3f);

            // Rebuild the view from the updated camera position.
            const Mat4 transform = camera.getProjectionMatrix() * camera.getViewMatrix() * model;

            buffer.clear(Pixel{0, 0, 0});

            std::array<Vec4, 8> clipVertices{};

            // Transform each cube vertex into clip space.
            for (std::size_t index = 0; index < vertices.size(); ++index) {
                clipVertices[index] = transform * vertices[index];
            }

            // Clip and draw each edge independently.
            for (const Edge& edge : edges) {
                Vec4 start = clipVertices[edge.start];
                Vec4 end = clipVertices[edge.end];

                if (!clipLine(start, end)) {
                    continue;
                }

                if (start.w <= 0.0f || end.w <= 0.0f) {
                    continue;
                }

                const float startNdcX = start.x / start.w;
                const float startNdcY = start.y / start.w;

                const float endNdcX = end.x / end.w;
                const float endNdcY = end.y / end.w;

                const float startScreenX =
                    (startNdcX + 1.0f) * 0.5f *
                    static_cast<float>(buffer.getWidth() - 1);

                const float startScreenY =
                    (1.0f - startNdcY) * 0.5f *
                    static_cast<float>(buffer.getHeight() - 1);

                const float endScreenX =
                    (endNdcX + 1.0f) * 0.5f *
                    static_cast<float>(buffer.getWidth() - 1);

                const float endScreenY =
                    (1.0f - endNdcY) * 0.5f *
                    static_cast<float>(buffer.getHeight() - 1);

                drawLine(
                    buffer,
                    static_cast<int>(std::lround(startScreenX)),
                    static_cast<int>(std::lround(startScreenY)),
                    static_cast<int>(std::lround(endScreenX)),
                    static_cast<int>(std::lround(endScreenY)),
                    Pixel{255, 255, 255}
                );
            }

            display.present(buffer);

            // Limit the frame rate.
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