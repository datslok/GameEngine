#include "engine/application.h"
#include "games/demo/demo_game.h"
#include <SDL3/SDL_main.h>

#include <exception>
#include <iostream>

/*
* Picks the game and starts the engine. Runtime errors are reported cleanly before the program exits.
* The game is declared first, so it outlives the Application that borrows it.
*/
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    try {
        DemoGame game{ControlMode::FreeCamera, true};
        Application application{"My Engine", 1920, 1080, game};
        application.run();
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
