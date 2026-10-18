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

// The file's text: a comment explaining it, then each current value whose text differs from its default.
std::string formatSettingsText(const std::vector<SettingEntry>& current, const std::vector<SettingEntry>& defaults);

struct ParsedSettings {
    std::vector<SettingEntry> entries;
    std::vector<std::string> warnings; // one per line that is not "key = value", with its line number
};

// Lines of "key = value"; blank lines and lines starting with # are skipped. Accepts \n and \r\n line ends.
ParsedSettings parseSettingsText(std::string_view text);

// Every field whose value is outside its hard limits, or whose slider range is not finite, ordered and inside them.
std::vector<std::string> findLimitProblems(const SettingsWalk& walk);

// The engine's settings under "engine" and the game's own (Game::visitSettings) under "game", as one tree.
SettingsWalk engineAndGameSettings(EngineSettings& settings, Game& game);

// The whole file, or nothing when it does not exist (no settings saved yet). Throws std::runtime_error when an
// existing file cannot be read or written.
std::optional<std::string> readSettingsFile(const char* path);
void writeSettingsFile(const char* path, const std::string& text);
