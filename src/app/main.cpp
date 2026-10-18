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
* both exit with 0 on success. --vulkan draws with the engine's own Vulkan renderer instead of SDL_GPU, for the game
* or the golden images, while phase 6b compares the two.
*/
int main(int argc, char* argv[]) {
    try {
        std::string goldenOption;
        bool vulkan = false;

        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];

            if (option == "--vulkan") {
                vulkan = true;
            }
            else if (option == "--golden-save" || option == "--golden-check") {
                goldenOption = option;
            }
            else {
                std::cerr << "Unknown option: " << option << '\n';
                return 1;
            }
        }

        EngineSettings settings;

        if (vulkan) {
            settings.graphicsApi = GraphicsApi::Vulkan;
        }

        if (!goldenOption.empty()) {
            const GoldenMode mode = goldenOption == "--golden-save" ? GoldenMode::Save : GoldenMode::Check;
            return runGoldenImages([] { return std::make_unique<DemoGame>(); }, mode, settings) == 0 ? 0 : 1;
        }

        DemoGame game{ControlMode::FirstPerson, true};
        Application application{"My Engine", 1920, 1080, game, settings};

        application.run();
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
