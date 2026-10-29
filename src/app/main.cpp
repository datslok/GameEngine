#include "app/cloth_yardstick_run.h"
#include "app/game_registry.h"
#include "core/command_registry.h"
#include "engine/application.h"
#include "engine/golden_capture.h"
#include "platform/command_socket.h"
#include <SDL3/SDL_main.h>

#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    /*
    * The port --send talks to: --port's, else the one a game started from this folder listens on (settings.cfg's
    * engine.commandPort, when saved there), else the default. A game started with --port elsewhere needs --port here.
    */
    std::uint16_t sendingPort(const LaunchOptions& options) {
        if (options.port) {
            return *options.port;
        }

        EngineSettings settings;

        if (const std::optional<std::string> saved = readSettingsFile(savedSettingsPath)) {
            const SettingsWalk walk = engineSettings(settings);
            applySettings(separateSections(parseSettingsText(*saved).entries, walk).own, walk, StartupFields::Apply);
        }

        return static_cast<std::uint16_t>(settings.commandPort);
    }

    // A script's lines, as the file holds them (Application::queueCommands skips blanks and comments).
    std::vector<std::string> readScript(const std::string& path) {
        std::ifstream file{path};

        if (!file) {
            throw std::runtime_error("Could not read the script " + path);
        }

        std::vector<std::string> lines;
        for (std::string line; std::getline(file, line);) {
            lines.push_back(line);
        }

        return lines;
    }
}

/*
* Picks the game and starts the engine. Runtime errors are reported cleanly before the program exits.
* The game is made first, so it outlives the Application that borrows it.
* --game <name> picks a game (app/game_registry.h; the light lab without it). --golden-save captures the golden images as
* references, --golden-check compares with them (engine/golden.h), for the named game or else for every game; both exit
* with 0 on success. --cloth-yardstick (or --cloth-yardstick-quick, or --cloth-yardstick-dress) measures the cloth solver (app/cloth_yardstick_run.h) instead of playing.
* --set, --run and --port start the game with settings, a script and a port of its own; --send sends a command to a
* game already running, prints its reply and exits with 0 when it worked, 1 when not.
*/
int main(int argc, char* argv[]) {
    try {
        const LaunchOptions options = parseLaunchOptions(std::vector<std::string>(argv + 1, argv + argc));

        if (options.send) {
            const CommandReply reply = decodeReply(sendCommand(sendingPort(options), joinCommandWords(*options.send)));
            (reply.ok ? std::cout : std::cerr) << reply.text << (reply.text.empty() ? "" : "\n");
            return reply.ok ? 0 : 1;
        }

        if (options.clothYardstick) {
            runClothYardstick(std::cout, *options.clothYardstick);
            return 0;
        }

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
        // --port is the game's port as well as --send's.
        std::vector<SettingEntry> launchSettings = options.settings;
        if (options.port) {
            launchSettings.push_back({"engine.commandPort", std::to_string(*options.port)});
        }

        const std::vector<std::string> script = options.script ? readScript(*options.script) : std::vector<std::string>{};
        Application application{title.c_str(), 1920, 1080, *game, EngineSettings{}, SavedSettings::Load, launchSettings};
        application.queueCommands(script);
        application.run();
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
