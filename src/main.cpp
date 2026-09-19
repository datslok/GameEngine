#include "gpu_display.h"
#include "mat4.h"

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

        GpuDisplay display{"GPU Renderer", width, height};

        const Uint64 startTime = SDL_GetTicksNS();

        while (display.processEvents()) {
            const double elapsedSeconds =
                static_cast<double>(SDL_GetTicksNS() - startTime) /
                1'000'000'000.0;

            // Rotate at one radian per second.
            const float angle = static_cast<float>(
                std::fmod(
                    elapsedSeconds,
                    2.0 * std::numbers::pi_v<double>
                )
            );

            // Correct for the window's aspect ratio after rotation.
            const Mat4 aspectCorrection = Mat4::scaling(
                static_cast<float>(height) /
                    static_cast<float>(width),
                1.0f,
                1.0f
            );

            const Mat4 transform =
                aspectCorrection * Mat4::rotationZ(angle);

            display.drawQuad(transform, 0.08f, 0.12f, 0.20f);
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