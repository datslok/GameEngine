#include "display.h"
#include "pixelbuffer.h"
#include "rasterizer.h"
#include "mat4.h"
#include "vec3.h"
#include "vec4.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <vector>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <numbers>

#include "camera.h"
#include "camera_controller.h"
#include "clipper.h"
#include "depthbuffer.h"
#include "mesh.h"

struct ScreenPoint {
    int x;
    int y;
    float depth;
};

ScreenPoint projectToScreen(
    const Vec4& point,
    std::size_t width,
    std::size_t height
){
    const float ndcX = point.x / point.w;
    const float ndcY = point.y / point.w;
    const float ndcZ = point.z / point.w;
    const float depth = (ndcZ + 1.0f) * 0.5f;

    const float screenX =
        (ndcX + 1.0f) * 0.5f *
        static_cast<float>(width - 1);

    const float screenY =
        (1.0f - ndcY) * 0.5f *
        static_cast<float>(height - 1);

    return ScreenPoint{
        static_cast<int>(std::lround(screenX)),
        static_cast<int>(std::lround(screenY)),
        depth
    };
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    try {
        PixelBuffer buffer{1280, 720};
        DepthBuffer depthBuffer{
            buffer.getWidth(),
            buffer.getHeight()
        };
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

        const Mesh cube = Mesh::cube();

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
            const Mat4 view = camera.getViewMatrix();
            const Mat4 modelView = view * model;

            const Mat4 transform =
                camera.getProjectionMatrix() * modelView;

            buffer.clear(Pixel{0, 0, 0});
            depthBuffer.clear();

            std::vector<Vec4> clipVertices(cube.vertices.size());

            // Transform each cube vertex into clip space.
            for (std::size_t index = 0; index < cube.vertices.size(); ++index){
                clipVertices[index] = transform * cube.vertices[index];
            }

            // Render the filled cube faces.
            for (const Triangle& triangle : cube.triangles){
                    const Vec4 viewFirst = modelView * cube.vertices[triangle.first];

                    const Vec4 viewSecond = modelView * cube.vertices[triangle.second];

                    const Vec4 viewThird = modelView * cube.vertices[triangle.third];

                    const Vec3 edgeFirst{
                        viewSecond.x - viewFirst.x,
                        viewSecond.y - viewFirst.y,
                        viewSecond.z - viewFirst.z
                    };

                    const Vec3 edgeSecond{
                        viewThird.x - viewFirst.x,
                        viewThird.y - viewFirst.y,
                        viewThird.z - viewFirst.z
                    };

                    const Vec3 normal = edgeFirst.cross(edgeSecond);

                    const Vec3 toCamera{
                        -viewFirst.x,
                        -viewFirst.y,
                        -viewFirst.z
                    };

                    if (normal.dot(toCamera) <= 0.0f) {
                        continue;
                    }

                const std::vector<Vec4> clippedPolygon =
                    clipTriangle(
                        clipVertices[triangle.first],
                        clipVertices[triangle.second],
                        clipVertices[triangle.third]
                    );

                // A clipped triangle may become a polygon.
                // Triangulate it using a triangle fan.
                for (std::size_t index = 1;
                    index + 1 < clippedPolygon.size();
                    ++index) {

                    const Vec4& first = clippedPolygon[0];
                    const Vec4& second = clippedPolygon[index];
                    const Vec4& third = clippedPolygon[index + 1];

                    if (first.w <= 0.0f ||
                        second.w <= 0.0f ||
                        third.w <= 0.0f) {
                        continue;
                    }

                    const ScreenPoint screenFirst =
                        projectToScreen(
                            first,
                            buffer.getWidth(),
                            buffer.getHeight()
                        );

                    const ScreenPoint screenSecond =
                        projectToScreen(
                            second,
                            buffer.getWidth(),
                            buffer.getHeight()
                        );

                    const ScreenPoint screenThird =
                        projectToScreen(
                            third,
                            buffer.getWidth(),
                            buffer.getHeight()
                        );

                    fillTriangleDepth(
                        buffer,
                        depthBuffer,

                        screenFirst.x,
                        screenFirst.y,
                        screenFirst.depth,

                        screenSecond.x,
                        screenSecond.y,
                        screenSecond.depth,

                        screenThird.x,
                        screenThird.y,
                        screenThird.depth,

                        Pixel{80, 120, 220}
                    );
                }
            }

            // Clip and draw each edge independently.
            for (const Edge& edge : cube.edges){
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