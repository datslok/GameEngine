#pragma once

#include "engine/game.h"
#include "engine/golden_capture.h"
#include "engine/settings_file.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/*
* Every game the program can run, by a short name for the command line ("demo", "lightlab"). Each entry holds a function
* that makes a fresh game rather than a game itself, since the golden images need a new one for every view. The names
* are also the games' folders under tests/golden/<GPU>/, so they follow the golden view names' rules.
*/
struct GameEntry {
    const char* name;
    std::unique_ptr<Game> (*make)();
};

// In the order help text lists them; the first is the one a plain start runs.
std::span<const GameEntry> registeredGames();
const GameEntry& defaultGame();

// The entry with this name, or nullptr.
const GameEntry* findGame(std::string_view name);

// "demo, lightlab": the names, for help and error messages.
std::string gameNames();

/*
* What the command line asks for: a game by name (--game <name>; none named means the default game, or every game for
* the golden images) and optionally the golden images instead of playing (--golden-save, --golden-check), or the cloth
* yardstick (--cloth-yardstick, --cloth-yardstick-quick for development, or --cloth-yardstick-dress for the dress-sized
* test alone: app/cloth_yardstick_run.h), which plays no game and takes no other option. Throws std::invalid_argument,
* with the valid choices in the message, for an unknown option or game.
*/
enum class YardstickRun {
    Full,
    Quick,
    Dress,
};

/*
* The command options (core/command_registry.h): --set key=value (any number) sets settings over settings.cfg before
* anything reads them, start-up ones too; --run <file> runs a script of commands, one a line, at the first frame; --port
* is the port --send uses (engine.commandPort's default without it). --send takes every word after it as one command
* for a game already running, prints its reply and exits, so it takes nothing but --port before it.
*/
struct LaunchOptions {
    const GameEntry* game = nullptr;
    std::optional<GoldenMode> golden;
    std::optional<YardstickRun> clothYardstick;
    std::vector<SettingEntry> settings;
    std::optional<std::string> script;
    std::optional<std::uint16_t> port;
    std::optional<std::vector<std::string>> send;
};
LaunchOptions parseLaunchOptions(const std::vector<std::string>& arguments);
