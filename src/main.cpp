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

        struct ScreenPoint {
            int x = 0;
            int y = 0;
            bool visible = false;
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

            std::array<ScreenPoint, 8> screenPoints{};

            // Project the vertices into pixel coordinates.
            for (std::size_t index = 0; index < vertices.size(); ++index) {
                const Vec4 projected = transform * vertices[index];

                if (projected.w > 0.0f &&
                    projected.x >= -projected.w &&
                    projected.x <= projected.w &&
                    projected.y >= -projected.w &&
                    projected.y <= projected.w &&
                    projected.z >= -projected.w &&
                    projected.z <= projected.w) {

                    const float ndcX = projected.x / projected.w;
                    const float ndcY = projected.y / projected.w;

                    const float screenX =
                        (ndcX + 1.0f) * 0.5f *
                        static_cast<float>(buffer.getWidth() - 1);

                    const float screenY =
                        (1.0f - ndcY) * 0.5f *
                        static_cast<float>(buffer.getHeight() - 1);

                    screenPoints[index] = ScreenPoint{
                        static_cast<int>(std::lround(screenX)),
                        static_cast<int>(std::lround(screenY)),
                        true
                    };
                }
            }

            // Connect the projected vertices.
            for (const Edge& edge : edges) {
                const ScreenPoint& start = screenPoints[edge.start];
                const ScreenPoint& end = screenPoints[edge.end];

                if (start.visible && end.visible) {
                    drawLine(
                        buffer,
                        start.x, start.y,
                        end.x, end.y,
                        Pixel{255, 255, 255}
                    );
                }
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