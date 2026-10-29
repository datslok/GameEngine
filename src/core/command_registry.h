#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

/*
* Commands by name: the engine's text interface, for the command line (game.exe --send, --run) and for tools and AIs
* driving a running game. Each command is a name, a usage line, a sentence saying what it does and a handler given the
* words after the name. Whatever goes wrong comes back as an error reply, never as an exception, so one bad command
* cannot stop the game.
*
* The future editor's operations (placing, moving and deleting things, saving a level) are meant to be commands here too,
* with its buttons issuing them, so a script or an AI can do anything the editor's windows can.
*/

// What a command answers: whether it worked, and text for whoever sent it (a value, a list, or what went wrong).
struct CommandReply {
    bool ok = true;
    std::string text;
};

CommandReply commandOk(std::string text = {});
CommandReply commandError(std::string text);

/*
* A reply as it travels: "OK" or "ERROR", a newline, then the text. decodeReply reads it back; anything not starting with
* either status line is an error reply holding what came.
*/
std::string encodeReply(const CommandReply& reply);
CommandReply decodeReply(std::string_view bytes);

/*
* A line's words, split at spaces and tabs. Double quotes keep spaces in a word ("C:/my shots/a.png"), and inside them a
* backslash keeps the next quote or backslash; outside them a backslash is ordinary, so Windows paths need no quoting.
* Quotes join onto the word around them, as in a shell. Throws std::invalid_argument for a quote left open.
*/
std::vector<std::string> splitCommandWords(std::string_view line);

// Words back into a line splitCommandWords gives them back from, quoting only the words that need it.
std::string joinCommandWords(const std::vector<std::string>& words);

/*
* Up to count candidates near name, for "did you mean": first those containing it (ignoring case), then those a few
* edits away (from the whole candidate or its last dotted part). None when nothing is near, rather than a random guess.
*/
std::vector<std::string> closestNames(std::string_view name, const std::vector<std::string>& candidates, std::size_t count = 3);

// The whole text as a number, or std::invalid_argument naming what was wanted ("1.5x" is not 1.5; no infinities or NaN).
float floatArgument(const std::string& text, const char* what);
int intArgument(const std::string& text, const char* what);

// The words after a command's name.
using CommandArguments = std::vector<std::string>;

// A handler may also throw std::exception (std::invalid_argument for bad arguments): its message becomes the error reply.
using CommandHandler = std::function<CommandReply(const CommandArguments&)>;

struct CommandInfo {
    std::string name;
    std::string usage;       // "step [ticks]": the name and its arguments, optional ones in brackets
    std::string description; // one sentence
};

class CommandRegistry {
public:
    // Starts with help: "help" lists every command, "help <name>" shows one.
    CommandRegistry();

    // help refers to the registry it lives in, so a registry stays where it was made.
    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

    // Names are lower-case letters, digits and dashes, each used once. Throws std::invalid_argument otherwise.
    void add(std::string name, std::string usage, std::string description, CommandHandler handler);

    // Run one line: its first word names the command, the rest are its arguments.
    CommandReply execute(std::string_view line) const;

    // Every command, by name.
    std::vector<CommandInfo> commands() const;

private:
    struct Command {
        CommandInfo info;
        CommandHandler handler;
    };

    const Command* find(std::string_view name) const;
    CommandReply help(const CommandArguments& arguments) const;

    // Kept in name order, so help lists them alphabetically.
    std::vector<Command> registered;
};
