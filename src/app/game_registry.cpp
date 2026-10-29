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

namespace {
    std::string trimmedText(const std::string& text) {
        const std::size_t first = text.find_first_not_of(" \t");
        const std::size_t last = text.find_last_not_of(" \t");
        return first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
    }

    // "key=value", split at the first "=" (a value may hold more), each side trimmed and neither empty.
    SettingEntry launchSetting(const std::string& text) {
        const std::size_t equals = text.find('=');
        const SettingEntry entry = equals == std::string::npos
                                       ? SettingEntry{}
                                       : SettingEntry{trimmedText(text.substr(0, equals)), trimmedText(text.substr(equals + 1))};

        if (entry.key.empty() || entry.value.empty()) {
            throw std::invalid_argument("--set takes key=value, e.g. --set engine.targetFPS=60, not \"" + text + "\"");
        }

        return entry;
    }

    // 1 to 65535, digits only.
    std::uint16_t launchPort(const std::string& text) {
        const bool digits = !text.empty() && text.size() <= 5 && text.find_first_not_of("0123456789") == std::string::npos;
        const int port = digits ? std::stoi(text) : 0;

        if (port < 1 || port > 65535) {
            throw std::invalid_argument("--port takes a port from 1 to 65535, not \"" + text + "\"");
        }

        return static_cast<std::uint16_t>(port);
    }
}

/*
* Options may come in any order, each at most once (but --set), so "--golden-check --game lightlab" and the reverse mean
* the same. --send ends the options: what follows is its command.
*/
LaunchOptions parseLaunchOptions(const std::vector<std::string>& arguments) {
    LaunchOptions options;
    bool gameNamed = false;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];

        if (argument == "--send") {
            if (index + 1 >= arguments.size()) {
                throw std::invalid_argument("--send takes a command, e.g. --send get engine.targetFPS");
            }

            options.send = std::vector<std::string>(arguments.begin() + static_cast<std::ptrdiff_t>(index) + 1, arguments.end());
            break;
        } else if (argument == "--set" || argument == "--run" || argument == "--port") {
            if (index + 1 >= arguments.size()) {
                throw std::invalid_argument(argument + " needs a value after it");
            }

            const std::string& value = arguments[++index];

            if (argument == "--set") {
                options.settings.push_back(launchSetting(value));
            } else if (argument == "--run") {
                if (options.script) {
                    throw std::invalid_argument("Give --run once");
                }
                options.script = value;
            } else {
                if (options.port) {
                    throw std::invalid_argument("Give --port once");
                }
                options.port = launchPort(value);
            }
        } else if (argument == "--game") {
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
                                        ">, --golden-save, --golden-check, --cloth-yardstick, --cloth-yardstick-quick, --cloth-yardstick-dress, "
                                        "--set <key=value>, --run <script>, --port <port>, --send <command...>");
        }
    }

    if (options.clothYardstick && (gameNamed || options.golden)) {
        throw std::invalid_argument("The cloth yardstick plays no game: give it alone");
    }

    // Golden images and the yardstick use the defaults, so no setting or script may change what they measure.
    if ((options.golden || options.clothYardstick) && (!options.settings.empty() || options.script)) {
        throw std::invalid_argument("--set and --run play a game: they do not go with the golden images or the cloth yardstick");
    }

    if (options.send && (gameNamed || options.golden || options.clothYardstick || !options.settings.empty() || options.script)) {
        throw std::invalid_argument("--send talks to a game already running: only --port may come before it");
    }

    return options;
}
