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

        assert(std::string{defaultGame().name} == "lightlab");
        assert(findGame("demo") != nullptr);
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
        assert(!save.clothYardstick);

        const LaunchOptions yardstick = parseLaunchOptions({"--cloth-yardstick"});
        assert(yardstick.clothYardstick == YardstickRun::Full);
        assert(yardstick.game == nullptr && !yardstick.golden);

        const LaunchOptions quick = parseLaunchOptions({"--cloth-yardstick-quick"});
        assert(quick.clothYardstick == YardstickRun::Quick);
        assert(quick.game == nullptr && !quick.golden);

        const LaunchOptions dress = parseLaunchOptions({"--cloth-yardstick-dress"});
        assert(dress.clothYardstick == YardstickRun::Dress);
        assert(dress.game == nullptr && !dress.golden);
    }

    /*
    * The command options: --set (any number, each key=value, the value may hold spaces and =), --run with a script,
    * --port for the commands' port, and --send, which takes every word after it as the command to send to a running
    * game, quoting kept apart.
    */
    void testCommandOptions() {
        const LaunchOptions plain = parseLaunchOptions({});
        assert(plain.settings.empty());
        assert(!plain.script);
        assert(!plain.send);
        assert(!plain.port);

        const LaunchOptions set = parseLaunchOptions({"--set", "engine.targetFPS=60", "--game", "demo", "--set", "demo.title = a=b"});
        assert(set.game == findGame("demo"));
        assert(set.settings.size() == 2);
        assert(set.settings[0].key == "engine.targetFPS" && set.settings[0].value == "60");
        assert(set.settings[1].key == "demo.title" && set.settings[1].value == "a=b");

        const LaunchOptions script = parseLaunchOptions({"--run", "shots.txt", "--port", "5000"});
        assert(script.script == "shots.txt");
        assert(script.port == 5000);

        const LaunchOptions send = parseLaunchOptions({"--port", "5001", "--send", "screenshot", "C:/my shots/a.png", "--game"});
        assert(send.port == 5001);
        assert(send.send);
        assert((*send.send == std::vector<std::string>{"screenshot", "C:/my shots/a.png", "--game"}));
    }

    // Mistakes are reported, not guessed at.
    void testLaunchMistakes() {
        assert(throwsInvalidArgument({"--set"}));
        assert(throwsInvalidArgument({"--set", "engine.targetFPS"}));
        assert(throwsInvalidArgument({"--set", "=60"}));
        assert(throwsInvalidArgument({"--set", "engine.targetFPS="}));
        assert(throwsInvalidArgument({"--run"}));
        assert(throwsInvalidArgument({"--run", "a.txt", "--run", "b.txt"}));
        assert(throwsInvalidArgument({"--port"}));
        assert(throwsInvalidArgument({"--port", "0"}));
        assert(throwsInvalidArgument({"--port", "70000"}));
        assert(throwsInvalidArgument({"--port", "80x"}));
        assert(throwsInvalidArgument({"--send"}));
        assert(throwsInvalidArgument({"--game", "demo", "--send", "help"}));
        assert(throwsInvalidArgument({"--set", "a=b", "--send", "help"}));
        assert(throwsInvalidArgument({"--cloth-yardstick", "--set", "a=b"}));
        assert(throwsInvalidArgument({"--golden-check", "--run", "a.txt"}));
        assert(throwsInvalidArgument({"--game"}));
        assert(throwsInvalidArgument({"--game", "nonsense"}));
        assert(throwsInvalidArgument({"--fly"}));
        assert(throwsInvalidArgument({"--golden-save", "--golden-check"}));
        assert(throwsInvalidArgument({"--game", "demo", "--game", "lightlab"}));
        assert(throwsInvalidArgument({"--cloth-yardstick", "--game", "clothlab"}));
        assert(throwsInvalidArgument({"--golden-check", "--cloth-yardstick"}));
        assert(throwsInvalidArgument({"--cloth-yardstick", "--cloth-yardstick"}));
        assert(throwsInvalidArgument({"--cloth-yardstick", "--cloth-yardstick-quick"}));
        assert(throwsInvalidArgument({"--cloth-yardstick-quick", "--game", "clothlab"}));
        assert(throwsInvalidArgument({"--cloth-yardstick-dress", "--cloth-yardstick"}));
        assert(throwsInvalidArgument({"--golden-save", "--cloth-yardstick-dress"}));
    }
}

void testGameRegistry() {
    testEveryGameIsUsable();
    testLaunchOptions();
    testCommandOptions();
    testLaunchMistakes();
}
