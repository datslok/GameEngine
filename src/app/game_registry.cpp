#include "app/game_registry.h"

#include "games/clothlab/cloth_lab_game.h"
#include "games/demo/demo_game.h"
#include "games/lightlab/light_lab_game.h"

#include <array>
#include <stdexcept>

namespace {
    // Each starts in first person, the way a player meets it; F2 and F3 switch views.
    std::unique_ptr<Game> makeDemo() {
        return std::make_unique<DemoGame>(ControlMode::FirstPerson, true);
    }

    std::unique_ptr<Game> makeLightLab() {
        return std::make_unique<LightLabGame>(ControlMode::FirstPerson, true);
    }

    std::unique_ptr<Game> makeClothLab() {
        return std::make_unique<ClothLabGame>(ControlMode::FirstPerson, true);
    }

    // Adding a game is one line here. Plain function pointers: the entries capture nothing, so the table is built at
    // compile time.
    constexpr std::array<GameEntry, 3> games{{
        {"lightlab", makeLightLab},
        {"demo", makeDemo},
        {"clothlab", makeClothLab}
    }};
}

std::span<const GameEntry> registeredGames() {
    return games;
}

const GameEntry& defaultGame() {
    return games.front();
}

const GameEntry* findGame(std::string_view name) {
    for (const GameEntry& entry : games) {
        if (name == entry.name) {
            return &entry;
        }
    }

    return nullptr;
}

std::string gameNames() {
    std::string names;

    for (const GameEntry& entry : games) {
        names += (names.empty() ? "" : ", ") + std::string{entry.name};
    }

    return names;
}

/*
* Options may come in any order, each at most once, so "--golden-check --game lightlab" and the reverse mean the same.
*/
LaunchOptions parseLaunchOptions(const std::vector<std::string>& arguments) {
    LaunchOptions options;
    bool gameNamed = false;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];

        if (argument == "--game") {
            if (gameNamed || index + 1 >= arguments.size()) {
                throw std::invalid_argument("--game takes one game name, once: " + gameNames());
            }

            const std::string& name = arguments[++index];
            options.game = findGame(name);
            gameNamed = true;

            if (options.game == nullptr) {
                throw std::invalid_argument("No game called \"" + name + "\". The games are: " + gameNames());
            }
        } else if (argument == "--golden-save" || argument == "--golden-check") {
            if (options.golden) {
                throw std::invalid_argument("Give --golden-save or --golden-check, not both");
            }

            options.golden = argument == "--golden-save" ? GoldenMode::Save : GoldenMode::Check;
        } else if (argument == "--cloth-yardstick" || argument == "--cloth-yardstick-quick" || argument == "--cloth-yardstick-dress") {
            if (options.clothYardstick) {
                throw std::invalid_argument("Give one of --cloth-yardstick, --cloth-yardstick-quick and --cloth-yardstick-dress, once");
            }

            options.clothYardstick = argument == "--cloth-yardstick"         ? YardstickRun::Full
                                     : argument == "--cloth-yardstick-quick" ? YardstickRun::Quick
                                                                             : YardstickRun::Dress;
        } else {
            throw std::invalid_argument("Unknown option \"" + argument + "\". Options: --game <" + gameNames() +
                                        ">, --golden-save, --golden-check, --cloth-yardstick, --cloth-yardstick-quick, --cloth-yardstick-dress");
        }
    }

    if (options.clothYardstick && (gameNamed || options.golden)) {
        throw std::invalid_argument("The cloth yardstick plays no game: give it alone");
    }

    return options;
}
