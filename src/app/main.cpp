#include "app/game_registry.h"
#include "engine/application.h"
#include "engine/golden_capture.h"
#include <SDL3/SDL_main.h>

#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

/*
* Picks the game and starts the engine. Runtime errors are reported cleanly before the program exits.
* The game is made first, so it outlives the Application that borrows it.
* --game <name> picks a game (app/game_registry.h; the demo without it). --golden-save captures the golden images as
* references, --golden-check compares with them (engine/golden.h), for the named game or else for every game; both exit
* with 0 on success.
*/
int main(int argc, char* argv[]) {
    try {
        const LaunchOptions options = parseLaunchOptions(std::vector<std::string>(argv + 1, argv + argc));

        if (options.golden) {
            int failures = 0;

            for (const GameEntry& entry : registeredGames()) {
                if (options.game == nullptr || options.game == &entry) {
                    failures += runGoldenImages(entry.make, entry.name, *options.golden);
                }
            }

            return failures == 0 ? 0 : 1;
        }

        const std::unique_ptr<Game> game = (options.game != nullptr ? *options.game : defaultGame()).make();
        const std::string title = std::string{"My Engine: "} + game->title();
        Application application{title.c_str(), 1920, 1080, *game};
        application.run();
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
