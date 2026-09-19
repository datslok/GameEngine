#include "gpu_display.h"
#include "camera.h"
#include "mat4.h"
#include "vec3.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cmath>
#include <exception>
#include <iostream>
#include <numbers>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    try {
        const int width = 1280;
        const int height = 720;

        GpuDisplay display{"GPU Cube", width, height};

        Camera camera{
            Vec3{0.0f, 0.0f, 0.0f},
            Vec3{0.0f, 0.0f, -5.0f},
            Vec3{0.0f, 1.0f, 0.0f},
            std::numbers::pi_v<float> / 3.0f,
            static_cast<float>(width) / static_cast<float>(height),
            0.1f,
            100.0f
        };

        // Convert our projection's depth range from [-1, 1] to [0, 1].
        // In clip space: new z = 0.5 * z + 0.5 * w.
        Mat4 depthCorrection = Mat4::identity();
        depthCorrection.values[2][2] = 0.5f;
        depthCorrection.values[2][3] = 0.5f;

        const Uint64 startTime = SDL_GetTicksNS();

        while (display.processEvents()) {
            const double elapsedSeconds =
                static_cast<double>(SDL_GetTicksNS() - startTime) /
                1'000'000'000.0;

            // Rotate at one radian per second, keeping the angle small.
            const float angle = static_cast<float>(
                std::fmod(
                    elapsedSeconds * 0.2, // radians per second
                    2.0 * std::numbers::pi_v<double>
                )
            );

            const Mat4 model =
                Mat4::translation(0.0f, 0.0f, -5.0f) *
                Mat4::rotationY(angle) *
                Mat4::rotationX(0.4f);

            const Mat4 transform =
                depthCorrection *
                camera.getProjectionMatrix() *
                camera.getViewMatrix() *
                model;

            display.drawMesh(transform, 0.08f, 0.12f, 0.20f);
        }
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}


/*

#include "application.h"

#include <SDL3/SDL_main.h>
#include <exception>
#include <iostream>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    try {
        Application application(1920, 1080);
        application.run();
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}

*/