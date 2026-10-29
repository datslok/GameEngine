#pragma once

#include "core/settings_visitor.h"
#include "engine/engine_settings.h"
#include "engine/game.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

/*
* Saving and loading settings as text, one "key = value" line per field, through the same visit() functions the debug
* UI draws from. Only values that differ from the defaults are saved, so a default improved later still reaches anyone
* who never changed it, and deleting a line returns that setting to its default.
*
* Loading is forgiving, since the file can be edited by hand and outlives renamed settings: a value past its limits is
* clamped, one that does not read as its kind is skipped, and an unknown key is ignored, each with a warning.
*/

// The file the engine loads at start-up and the debug UI saves to, in the working directory (the repo root, or the
// packaged game's folder).
inline constexpr const char* savedSettingsPath = "settings.cfg";

// One field's full key (its groups' keys and its own, joined with dots) and its value as text.
struct SettingEntry {
    std::string key;
    std::string value;
};

// Every field a walk visits, in visiting order. Floats are written as the shortest text that reads back exactly.
std::vector<SettingEntry> captureSettings(const SettingsWalk& walk);

// Start-up fields were read once already while the game runs; resetting to defaults then leaves them alone.
enum class StartupFields {
    Apply,
    Keep
};

// Set the walked fields named in entries. Returns a warning for each value clamped or skipped and each unknown key.
std::vector<std::string> applySettings(const std::vector<SettingEntry>& entries, const SettingsWalk& walk, StartupFields startup);

// The file's text: a comment explaining it, then each current value whose text differs from its default, then the
// kept entries as they are (other games' sections, see separateSections).
std::string formatSettingsText(const std::vector<SettingEntry>& current, const std::vector<SettingEntry>& defaults,
                               const std::vector<SettingEntry>& kept = {});

struct ParsedSettings {
    std::vector<SettingEntry> entries;
    std::vector<std::string> warnings; // one per line that is not "key = value", with its line number
};

// Lines of "key = value"; blank lines and lines starting with # are skipped. Accepts \n and \r\n line ends.
ParsedSettings parseSettingsText(std::string_view text);

/*
* Saved entries split by their first key part: own are in a section the walk has (or have no section at all, so a
* stray key is still reported), others in a section it lacks, which in one shared file means another game's. Loading
* applies own and keeps others untouched, and saving writes them back, so each game's settings survive the other's saves.
*/
struct SectionedSettings {
    std::vector<SettingEntry> own;
    std::vector<SettingEntry> others;
};
SectionedSettings separateSections(const std::vector<SettingEntry>& entries, const SettingsWalk& walk);

/*
* One field described for people and for commands: its key and value as the file saves them, what it takes ("number 0
* to 1 (slider 0 to 0.2) nits", "whole number 1 to 8", "true or false", "one of Vsync, Mailbox, Immediate"), and whether
* it is read only at start-up.
*/
struct SettingDescription {
    std::string key;
    std::string value;
    std::string kind;
    bool atStartup = false;
};

// Every field a walk visits, in visiting order (the order captureSettings gives).
std::vector<SettingDescription> describeSettings(const SettingsWalk& walk);

// What setting one field live did (setSetting).
enum class SettingOutcome {
    Set,
    Clamped,       // past its hard limits: set to the nearest limit
    Refused,       // not a value of its kind: left as it was
    Unknown,       // no field has the key (a group's name is not a field)
    AtStartupOnly  // read once at start-up, so changing it now would do nothing: left as it was
};

struct SettingChange {
    SettingOutcome outcome = SettingOutcome::Set;
    std::string value;   // the field's value afterwards, as text (empty when Unknown)
    std::string message; // why, unless Set
};

// Set one field of a running game by its full key, as the settings file would, but saying exactly what happened.
SettingChange setSetting(const SettingsWalk& walk, const std::string& key, const std::string& value);

// Every field whose value is outside its hard limits, or whose slider range is not finite, ordered and inside them.
std::vector<std::string> findLimitProblems(const SettingsWalk& walk);

// The engine's settings under "engine" and the game's own (Game::visitSettings) under its title ("demo", "lightLab"),
// as one tree. Each game has a section of its own, so one game's saved settings never land on another's.
SettingsWalk engineAndGameSettings(EngineSettings& settings, Game& game);

// The engine's settings alone, under "engine", as engineAndGameSettings walks them (no game needed: --send's port).
SettingsWalk engineSettings(EngineSettings& settings);

// The whole file, or nothing when it does not exist (no settings saved yet). Throws std::runtime_error when an
// existing file cannot be read or written.
std::optional<std::string> readSettingsFile(const char* path);
void writeSettingsFile(const char* path, const std::string& text);
