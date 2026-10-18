#include "engine/application.h"
#include "engine/golden_capture.h"
#include "games/demo/demo_game.h"
#include <SDL3/SDL_main.h>

#include <exception>
#include <iostream>
#include <memory>
#include <string>

/*
* Picks the game and starts the engine. Runtime errors are reported cleanly before the program exits.
* The game is declared first, so it outlives the Application that borrows it.
* --golden-save captures the demo's golden images as references, --golden-check compares with them (engine/golden.h);
* both exit with 0 on success.
*/
int main(int argc, char* argv[]) {
    try {
        const std::string option = argc > 1 ? argv[1] : "";

        if (option == "--golden-save" || option == "--golden-check") {
            const GoldenMode mode = option == "--golden-save" ? GoldenMode::Save : GoldenMode::Check;
            return runGoldenImages([] { return std::make_unique<DemoGame>(); }, mode) == 0 ? 0 : 1;
        }

        DemoGame game{ControlMode::FirstPerson, true};
        Application application{"My Engine", 1920, 1080, game};
        application.run();
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
