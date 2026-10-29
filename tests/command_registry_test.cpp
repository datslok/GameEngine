#include "core/command_registry.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    bool contains(const std::string& text, const std::string& part) {
        return text.find(part) != std::string::npos;
    }

    bool splitThrows(const std::string& line) {
        try {
            splitCommandWords(line);
        }
        catch (const std::invalid_argument&) {
            return true;
        }

        return false;
    }

    // Words split at spaces and tabs; double quotes keep spaces, and a backslash inside them keeps a quote or itself.
    void testWordsAreSplit() {
        assert(splitCommandWords("") == std::vector<std::string>{});
        assert(splitCommandWords("   \t ") == std::vector<std::string>{});
        assert((splitCommandWords("set engine.targetFPS 60") == std::vector<std::string>{"set", "engine.targetFPS", "60"}));
        assert((splitCommandWords("  get\tfog  ") == std::vector<std::string>{"get", "fog"}));
        assert((splitCommandWords("screenshot \"C:/my shots/a.png\"") == std::vector<std::string>{"screenshot", "C:/my shots/a.png"}));
        assert((splitCommandWords("say \"a \\\"quoted\\\" word\" \"back\\\\slash\"") ==
                std::vector<std::string>{"say", "a \"quoted\" word", "back\\slash"}));
        assert((splitCommandWords("empty \"\"") == std::vector<std::string>{"empty", ""}));
        // Quotes join onto the word around them, as in a shell.
        assert((splitCommandWords("glued\"to gether\"") == std::vector<std::string>{"gluedto gether"}));

        // Outside quotes a backslash is an ordinary character, so Windows paths need no quoting.
        assert((splitCommandWords("screenshot C:\\shots\\a.png") == std::vector<std::string>{"screenshot", "C:\\shots\\a.png"}));

        assert(splitThrows("say \"never closed"));
    }

    // Joining quotes only the words that need it, and splitting the result gives the words back.
    void testWordsAreJoined() {
        assert(joinCommandWords({"set", "engine.targetFPS", "60"}) == "set engine.targetFPS 60");

        const std::vector<std::string> awkward{"screenshot", "C:/my shots/a.png", "", "say \"hi\"", "C:\\path\\x.png", "back\\\"slash"};
        assert(splitCommandWords(joinCommandWords(awkward)) == awkward);
    }

    // A reply travels as a status line and its text.
    void testRepliesRoundTrip() {
        const CommandReply ok = commandOk("engine.targetFPS = 60");
        const CommandReply error = commandError("no such setting\nsecond line");

        assert(ok.ok && !error.ok);
        assert(encodeReply(ok) == "OK\nengine.targetFPS = 60");

        const CommandReply okBack = decodeReply(encodeReply(ok));
        const CommandReply errorBack = decodeReply(encodeReply(error));
        assert(okBack.ok && okBack.text == ok.text);
        assert(!errorBack.ok && errorBack.text == error.text);
        assert(decodeReply(encodeReply(commandOk())).ok);
        assert(decodeReply(encodeReply(commandOk())).text.empty());

        // Anything else is not a reply the engine sent.
        assert(!decodeReply("").ok);
        assert(!decodeReply("HELLO\nthere").ok);
    }

    // Near names are offered for mistakes: those containing what was typed first, then the fewest edits away.
    void testClosestNames() {
        const std::vector<std::string> names{"engine.render.bloom.strength", "engine.render.bloom.maxLevels", "engine.targetFPS",
                                             "engine.clothOnGPU", "lightLab.fog.density"};

        const std::vector<std::string> bloom = closestNames("bloom", names);
        assert(bloom.size() == 2);
        assert(std::find(bloom.begin(), bloom.end(), "engine.render.bloom.strength") != bloom.end());

        const std::vector<std::string> typo = closestNames("engine.targetFSP", names);
        assert(!typo.empty() && typo.front() == "engine.targetFPS");

        // A typo in a long key's last part is matched by that part, with a part's allowance, not the whole key's: the
        // light lab's "solarHour" once also offered bloom's radius and the probes' debug switch.
        const std::vector<std::string> sun{"lightLab.sun.solarHours", "engine.render.bloom.radius", "engine.render.globalIllumination.showProbes"};
        assert(closestNames("lightLab.sun.solarHour", sun) == std::vector<std::string>{"lightLab.sun.solarHours"});

        // Case is ignored.
        assert(closestNames("CLOTHONGPU", names) == std::vector<std::string>{"engine.clothOnGPU"});

        // Nothing near enough gives nothing, rather than a random guess.
        assert(closestNames("xyzzy", names).empty());
        assert(closestNames("bloom", names, 1).size() == 1);
    }

    // Commands are found by name and given the words after it.
    void testCommandsRun() {
        CommandRegistry registry;
        std::vector<std::string> seen;
        registry.add("echo", "echo <words...>", "Repeats its words.", [&](const CommandArguments& arguments) {
            seen = arguments;
            return commandOk(joinCommandWords(arguments));
        });

        const CommandReply reply = registry.execute("echo one \"two three\"");
        assert(reply.ok);
        assert(reply.text == "one \"two three\"");
        assert((seen == std::vector<std::string>{"one", "two three"}));

        assert(registry.execute("echo").ok);
        assert(seen.empty());
    }

    // Mistakes come back as error replies, never as exceptions: an empty line, an unknown command (with near names),
    // a broken quote, and a handler that throws.
    void testMistakesAreReplies() {
        CommandRegistry registry;
        registry.add("screenshot", "screenshot <file.png>", "Saves the picture.", [](const CommandArguments&) {
            return commandOk();
        });
        registry.add("fail", "fail", "Throws.", [](const CommandArguments&) -> CommandReply {
            throw std::invalid_argument("fail needs nothing");
        });

        assert(!registry.execute("").ok);
        assert(!registry.execute("   ").ok);

        const CommandReply unknown = registry.execute("screnshot a.png");
        assert(!unknown.ok);
        assert(contains(unknown.text, "screnshot"));
        assert(contains(unknown.text, "screenshot"));
        assert(contains(unknown.text, "help"));

        const CommandReply quote = registry.execute("screenshot \"a.png");
        assert(!quote.ok);
        assert(contains(quote.text, "quote"));

        const CommandReply thrown = registry.execute("fail");
        assert(!thrown.ok);
        assert(thrown.text == "fail needs nothing");
    }

    // help lists every command with its usage, itself included, so an AI can find what exists; help <name> shows one.
    void testHelpListsEveryCommand() {
        CommandRegistry registry;
        registry.add("step", "step [ticks]", "Runs ticks while paused.", [](const CommandArguments&) {
            return commandOk();
        });
        registry.add("camera", "camera [x y z [tx ty tz]]", "Shows or places the camera.", [](const CommandArguments&) {
            return commandOk();
        });

        const std::vector<CommandInfo> listed = registry.commands();
        assert(listed.size() == 3);
        assert(listed[0].name == "camera" && listed[1].name == "help" && listed[2].name == "step");

        const CommandReply all = registry.execute("help");
        assert(all.ok);
        assert(contains(all.text, "step [ticks]"));
        assert(contains(all.text, "Runs ticks while paused."));
        assert(contains(all.text, "camera [x y z [tx ty tz]]"));
        assert(contains(all.text, "help [command]"));

        const CommandReply one = registry.execute("help step");
        assert(one.ok);
        assert(contains(one.text, "step [ticks]"));
        assert(!contains(one.text, "camera"));

        assert(!registry.execute("help nonsense").ok);
    }

    // Names are plain words, each used once.
    void testNamesAreChecked() {
        CommandRegistry registry;
        const CommandHandler nothing = [](const CommandArguments&) {
            return commandOk();
        };
        registry.add("save-settings", "save-settings", "Saves.", nothing);

        bool threw = false;
        try {
            registry.add("save-settings", "save-settings", "Again.", nothing);
        }
        catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);

        for (const char* bad : {"", "Save", "two words", "help"}) {
            threw = false;
            try {
                registry.add(bad, bad, "Bad.", nothing);
            }
            catch (const std::invalid_argument&) {
                threw = true;
            }
            assert(threw);
        }
    }

    // Numbers in arguments must be whole numbers: "1.5x" is not 1.5. Mistakes throw, naming what was wanted.
    void testNumberArguments() {
        assert(floatArgument("2.5", "height") == 2.5f);
        assert(floatArgument("-3", "height") == -3.0f);
        assert(intArgument("120", "ticks") == 120);

        for (const char* bad : {"", "1.5x", "abc", "nan", "inf"}) {
            bool threw = false;
            try {
                floatArgument(bad, "height");
            }
            catch (const std::invalid_argument& error) {
                threw = contains(error.what(), "height");
            }
            assert(threw);
        }

        bool threw = false;
        try {
            intArgument("1.5", "ticks");
        }
        catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }
}

void testCommandRegistry() {
    testWordsAreSplit();
    testWordsAreJoined();
    testRepliesRoundTrip();
    testClosestNames();
    testCommandsRun();
    testMistakesAreReplies();
    testHelpListsEveryCommand();
    testNamesAreChecked();
    testNumberArguments();
}
