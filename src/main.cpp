#include "display.h"
#include "pixelbuffer.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <exception>
#include <iostream>
#include "rasterizer.h"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    try {
        PixelBuffer buffer{800, 600};
        Display display{"My Engine", 800, 600};

        Uint64 targetFPS = 60;  // 0 means unlimited

        while (true) {
            const Uint64 frameStart = SDL_GetTicksNS();

            if (!display.processEvents()) {
                break;
            }

            // Update and draw your scene here.
            buffer.clear(Pixel{0, 0, 255});
            drawLine(buffer, 100, 250, 300, 100, Pixel{255, 0, 0});
            drawTriangleOutline(buffer, 400, 100, 200, 400, 600, 400, Pixel{255, 0, 0});
            fillTriangle(buffer, 400, 180, 300, 330, 500, 330, Pixel{0, 255, 0});

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