#include "core/command_registry.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {
    constexpr std::string_view okStatus = "OK\n";
    constexpr std::string_view errorStatus = "ERROR\n";

    bool isSpace(char character) {
        return character == ' ' || character == '\t';
    }

    // A word needs quotes when splitting would change it: it is empty, or holds a space, tab or quote.
    bool needsQuotes(const std::string& word) {
        return word.empty() || word.find_first_of(" \t\"") != std::string::npos;
    }

    std::string lowerCase(std::string_view text) {
        std::string lower(text);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return lower;
    }

    // The fewest single-letter insertions, deletions and substitutions turning one into the other (Levenshtein).
    std::size_t editDistance(std::string_view from, std::string_view to) {
        std::vector<std::size_t> row(to.size() + 1);

        for (std::size_t column = 0; column <= to.size(); ++column) {
            row[column] = column;
        }

        for (std::size_t line = 1; line <= from.size(); ++line) {
            std::size_t diagonal = row[0];
            row[0] = line;

            for (std::size_t column = 1; column <= to.size(); ++column) {
                const std::size_t above = row[column];
                const std::size_t substitution = diagonal + (from[line - 1] == to[column - 1] ? 0 : 1);
                row[column] = std::min({above + 1, row[column - 1] + 1, substitution});
                diagonal = above;
            }
        }

        return row[to.size()];
    }

    std::string_view lastPart(std::string_view key) {
        const std::size_t dot = key.rfind('.');
        return dot == std::string_view::npos ? key : key.substr(dot + 1);
    }

    bool isCommandName(const std::string& name) {
        return !name.empty() && std::all_of(name.begin(), name.end(), [](char character) {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-';
        });
    }

    template<typename Number>
    Number numberArgument(const std::string& text, const char* what, const char* kind) {
        Number value{};
        const char* end = text.data() + text.size();
        const std::from_chars_result result = std::from_chars(text.data(), end, value);

        if (text.empty() || result.ec != std::errc{} || result.ptr != end) {
            throw std::invalid_argument(std::string{what} + " must be " + kind + ", not \"" + text + "\"");
        }

        return value;
    }
}

CommandReply commandOk(std::string text) {
    return {true, std::move(text)};
}

CommandReply commandError(std::string text) {
    return {false, std::move(text)};
}

std::string encodeReply(const CommandReply& reply) {
    return std::string{reply.ok ? okStatus : errorStatus} + reply.text;
}

CommandReply decodeReply(std::string_view bytes) {
    if (bytes.substr(0, okStatus.size()) == okStatus) {
        return commandOk(std::string{bytes.substr(okStatus.size())});
    }

    if (bytes.substr(0, errorStatus.size()) == errorStatus) {
        return commandError(std::string{bytes.substr(errorStatus.size())});
    }

    return commandError("Not a reply from the engine: \"" + std::string{bytes} + "\"");
}

/*
* One pass, one character at a time: a word starts at the first character that is not a space and ends at the first
* space outside quotes. A word made only of quotes ("") is still a word, so wordStarted is kept apart from its text.
*/
std::vector<std::string> splitCommandWords(std::string_view line) {
    std::vector<std::string> words;
    std::string word;
    bool wordStarted = false;
    bool quoted = false;

    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];

        if (quoted) {
            if (character == '\\' && index + 1 < line.size() && (line[index + 1] == '"' || line[index + 1] == '\\')) {
                word += line[++index];
            } else if (character == '"') {
                quoted = false;
            } else {
                word += character;
            }
        } else if (isSpace(character)) {
            if (wordStarted) {
                words.push_back(word);
                word.clear();
                wordStarted = false;
            }
        } else if (character == '"') {
            quoted = true;
            wordStarted = true;
        } else {
            word += character;
            wordStarted = true;
        }
    }

    if (quoted) {
        throw std::invalid_argument("A double quote was left open: " + std::string{line});
    }

    if (wordStarted) {
        words.push_back(word);
    }

    return words;
}

std::string joinCommandWords(const std::vector<std::string>& words) {
    std::string line;

    for (const std::string& word : words) {
        if (!line.empty()) {
            line += ' ';
        }

        if (!needsQuotes(word)) {
            line += word;
            continue;
        }

        line += '"';
        for (const char character : word) {
            if (character == '"' || character == '\\') {
                line += '\\';
            }
            line += character;
        }
        line += '"';
    }

    return line;
}

/*
* Containing the name is the strongest hint (a key's last parts typed alone). Otherwise up to a third of what is compared
* may be wrong, at least two letters (a swapped pair is two edits): of the whole name against the whole candidate, or of
* its last dotted part against the candidate's, each with its own length's allowance.
*/
std::vector<std::string> closestNames(std::string_view name, const std::vector<std::string>& candidates, std::size_t count) {
    const std::string lowerName = lowerCase(name);
    const std::string_view namePart = lastPart(lowerName);
    std::vector<std::string> containing;
    std::vector<std::pair<std::size_t, std::string>> near;

    const auto allowance = [](std::string_view text) {
        return std::max<std::size_t>(2, text.size() / 3);
    };

    for (const std::string& candidate : candidates) {
        const std::string lowerCandidate = lowerCase(candidate);

        if (!lowerName.empty() && lowerCandidate.find(lowerName) != std::string::npos) {
            containing.push_back(candidate);
            continue;
        }

        const std::size_t whole = editDistance(lowerName, lowerCandidate);
        const std::size_t part = editDistance(namePart, lastPart(lowerCandidate));

        if (whole <= allowance(lowerName) || part <= allowance(namePart)) {
            near.emplace_back(std::min(whole, part), candidate);
        }
    }

    std::stable_sort(near.begin(), near.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    std::vector<std::string> closest = containing;
    for (const auto& [distance, candidate] : near) {
        closest.push_back(candidate);
    }

    if (closest.size() > count) {
        closest.resize(count);
    }

    return closest;
}

float floatArgument(const std::string& text, const char* what) {
    const float value = numberArgument<float>(text, what, "a number");

    if (!std::isfinite(value)) {
        throw std::invalid_argument(std::string{what} + " must be a finite number, not \"" + text + "\"");
    }

    return value;
}

int intArgument(const std::string& text, const char* what) {
    return numberArgument<int>(text, what, "a whole number");
}

CommandRegistry::CommandRegistry() {
    registered.push_back({{"help", "help [command]", "Lists every command, or shows one command's usage."},
                          [this](const CommandArguments& arguments) {
                              return help(arguments);
                          }});
}

void CommandRegistry::add(std::string name, std::string usage, std::string description, CommandHandler handler) {
    if (!isCommandName(name)) {
        throw std::invalid_argument("Command names are lower-case letters, digits and dashes: \"" + name + "\"");
    }

    if (find(name) != nullptr) {
        throw std::invalid_argument("A command called \"" + name + "\" exists already");
    }

    const auto position = std::lower_bound(registered.begin(), registered.end(), name, [](const Command& command, const std::string& wanted) {
        return command.info.name < wanted;
    });
    registered.insert(position, {{std::move(name), std::move(usage), std::move(description)}, std::move(handler)});
}

CommandReply CommandRegistry::execute(std::string_view line) const {
    try {
        CommandArguments words = splitCommandWords(line);

        if (words.empty()) {
            return commandError("No command given. \"help\" lists them all.");
        }

        const Command* command = find(words.front());

        if (command == nullptr) {
            std::vector<std::string> names;
            for (const Command& each : registered) {
                names.push_back(each.info.name);
            }

            std::string message = "No command called \"" + words.front() + "\".";
            const std::vector<std::string> near = closestNames(words.front(), names);

            for (std::size_t index = 0; index < near.size(); ++index) {
                message += (index == 0 ? " Did you mean " : ", ") + near[index];
            }

            return commandError(message + (near.empty() ? "" : "?") + " \"help\" lists them all.");
        }

        words.erase(words.begin());
        return command->handler(words);
    }
    catch (const std::exception& error) {
        return commandError(error.what());
    }
}

std::vector<CommandInfo> CommandRegistry::commands() const {
    std::vector<CommandInfo> infos;

    for (const Command& command : registered) {
        infos.push_back(command.info);
    }

    return infos;
}

const CommandRegistry::Command* CommandRegistry::find(std::string_view name) const {
    for (const Command& command : registered) {
        if (command.info.name == name) {
            return &command;
        }
    }

    return nullptr;
}

// Each command's usage on a line of its own, its description indented below it.
CommandReply CommandRegistry::help(const CommandArguments& arguments) const {
    if (arguments.size() > 1) {
        return commandError("help takes at most one command's name");
    }

    std::string text;

    for (const Command& command : registered) {
        if (arguments.empty() || command.info.name == arguments.front()) {
            text += (text.empty() ? "" : "\n") + command.info.usage + "\n    " + command.info.description;
        }
    }

    if (text.empty()) {
        return commandError("No command called \"" + arguments.front() + "\". \"help\" lists them all.");
    }

    return commandOk(text);
}
