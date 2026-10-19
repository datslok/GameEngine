#include "app/game_registry.h"
#include "engine/golden.h"

#include <cassert>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    bool throwsInvalidArgument(const std::vector<std::string>& arguments) {
        try {
            parseLaunchOptions(arguments);
        }
        catch (const std::invalid_argument&) {
            return true;
        }

        return false;
    }

    // Names are command-line words and golden folders: unique and plain. Each entry makes a game with a title of its own.
    void testEveryGameIsUsable() {
        std::set<std::string> names;
        std::set<std::string> titles;

        assert(registeredGames().size() >= 2);

        for (const GameEntry& entry : registeredGames()) {
            assert(isGoldenViewName(entry.name));
            assert(names.insert(entry.name).second);

            const std::unique_ptr<Game> game = entry.make();
            assert(game != nullptr);
            assert(!std::string{game->title()}.empty());
            assert(titles.insert(game->title()).second);
        }

        assert(std::string{defaultGame().name} == "demo");
        assert(findGame("lightlab") != nullptr);
        assert(findGame("nonsense") == nullptr);
        assert(gameNames().find("lightlab") != std::string::npos);
    }

    // No options plays the default; --game picks one; the golden options may come with or without a game.
    void testLaunchOptions() {
        const LaunchOptions plain = parseLaunchOptions({});
        assert(plain.game == nullptr);
        assert(!plain.golden);

        const LaunchOptions lab = parseLaunchOptions({"--game", "lightlab"});
        assert(lab.game == findGame("lightlab"));
        assert(!lab.golden);

        const LaunchOptions check = parseLaunchOptions({"--golden-check"});
        assert(check.game == nullptr);
        assert(check.golden == GoldenMode::Check);

        const LaunchOptions save = parseLaunchOptions({"--golden-save", "--game", "demo"});
        assert(save.game == findGame("demo"));
        assert(save.golden == GoldenMode::Save);
    }

    // Mistakes are reported, not guessed at.
    void testLaunchMistakes() {
        assert(throwsInvalidArgument({"--game"}));
        assert(throwsInvalidArgument({"--game", "nonsense"}));
        assert(throwsInvalidArgument({"--fly"}));
        assert(throwsInvalidArgument({"--golden-save", "--golden-check"}));
        assert(throwsInvalidArgument({"--game", "demo", "--game", "lightlab"}));
    }
}

void testGameRegistry() {
    testEveryGameIsUsable();
    testLaunchOptions();
    testLaunchMistakes();
}
