#include <iostream>
#include "pixel.h"
#include "pixelbuffer.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << SDL_GetError() << '\n';
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("My Engine", 800, 600, 0);

    if (window == nullptr) {
        std::cerr << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }

    bool running = true;
    int exitCode = 0;

    while (running) {
        SDL_Event event;

        if (!SDL_WaitEvent(&event)) {
            std::cerr << SDL_GetError() << '\n';
            exitCode = 1;
            break;
        }

        if (event.type == SDL_EVENT_QUIT) {
            running = false;
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    return exitCode;
}