#include "engine/settings_file.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {
    /*
    * Tracks the key of whatever is being visited: each group adds its key and a dot, so a field's full key is its
    * groups' keys and its own. The kinds of field are handed on with their key.
    */
    class KeyedVisitor : public SettingsVisitor {
    protected:
        virtual void onFloat(const std::string& key, float& value, const FloatLimits& limits) = 0;
        virtual void onInt(const std::string& key, int& value, const IntLimits& limits) = 0;
        virtual void onBool(const std::string& key, bool& value, FieldApplies applies) = 0;
        virtual void onChoice(const std::string& key, int& index, const char* const* names, int count, FieldApplies applies) = 0;

        void visitFloat(const char* label, float& value, const FloatLimits& limits) override {
            onFloat(keyOf(label), value, limits);
        }

        void visitInt(const char* label, int& value, const IntLimits& limits) override {
            onInt(keyOf(label), value, limits);
        }

        void visitBool(const char* label, bool& value, FieldApplies applies) override {
            onBool(keyOf(label), value, applies);
        }

        void visitChoice(const char* label, int& index, const char* const* names, int count, FieldApplies applies) override {
            onChoice(keyOf(label), index, names, count, applies);
        }

        void visitGroup(const char* label, const std::function<void()>& contents) override {
            const std::size_t outerLength = prefix.size();
            prefix += settingKey(label);
            prefix += '.';
            contents();
            prefix.resize(outerLength);
        }

    private:
        std::string keyOf(const char* label) const {
            return prefix + settingKey(label);
        }

        std::string prefix;
    };

    // The shortest text that reads back as exactly this float (0.04f is "0.04", not 0.0399999991).
    std::string floatText(float value) {
        char buffer[64];
        const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return std::string(buffer, result.ptr);
    }

    std::string intText(int value) {
        return std::to_string(value);
    }

    const char* boolText(bool value) {
        return value ? "true" : "false";
    }

    // The whole text must be the number: "1.5x" is not 1.5. Infinities and NaN do not count as settings.
    std::optional<float> readFloat(const std::string& text) {
        float value = 0.0f;
        const char* end = text.data() + text.size();
        const std::from_chars_result result = std::from_chars(text.data(), end, value);

        if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(value)) {
            return std::nullopt;
        }

        return value;
    }

    std::optional<int> readInt(const std::string& text) {
        int value = 0;
        const char* end = text.data() + text.size();
        const std::from_chars_result result = std::from_chars(text.data(), end, value);

        if (result.ec != std::errc{} || result.ptr != end) {
            return std::nullopt;
        }

        return value;
    }

    // Records each field's value as text.
    class CapturingVisitor final : public KeyedVisitor {
    public:
        std::vector<SettingEntry> entries;

    protected:
        void onFloat(const std::string& key, float& value, const FloatLimits&) override {
            entries.push_back({key, floatText(value)});
        }

        void onInt(const std::string& key, int& value, const IntLimits&) override {
            entries.push_back({key, intText(value)});
        }

        void onBool(const std::string& key, bool& value, FieldApplies) override {
            entries.push_back({key, boolText(value)});
        }

        void onChoice(const std::string& key, int& index, const char* const* names, int, FieldApplies) override {
            entries.push_back({key, names[index]});
        }
    };

    // Sets each field named in the entries, clamped to its hard limits, and warns about everything it could not use.
    class ApplyingVisitor final : public KeyedVisitor {
    public:
        ApplyingVisitor(const std::vector<SettingEntry>& entries, StartupFields startup): startup(startup) {
            for (const SettingEntry& entry : entries) {
                values[entry.key] = entry.value;
            }
        }

        std::vector<std::string> warnings;

        // Values that did not read as their field's kind and were left out (the rest of the warnings only clamped).
        int refused = 0;

        // Keys no field claimed: settings that were renamed or removed, or typing mistakes.
        void warnAboutUnknownKeys() {
            for (const auto& [key, value] : values) {
                if (used.count(key) == 0) {
                    warnings.push_back(key + ": no such setting (ignored)");
                }
            }
        }

    protected:
        void onFloat(const std::string& key, float& value, const FloatLimits& limits) override {
            const std::string* text = find(key, limits.applies);

            if (text == nullptr) {
                return;
            }

            const std::optional<float> read = readFloat(*text);

            if (!read) {
                warnings.push_back(key + ": \"" + *text + "\" is not a number (kept " + floatText(value) + ")");
                ++refused;
                return;
            }

            value = clamped(key, *read, limits.min, limits.max, floatText);
        }

        void onInt(const std::string& key, int& value, const IntLimits& limits) override {
            const std::string* text = find(key, limits.applies);

            if (text == nullptr) {
                return;
            }

            const std::optional<int> read = readInt(*text);

            if (!read) {
                warnings.push_back(key + ": \"" + *text + "\" is not a whole number (kept " + intText(value) + ")");
                ++refused;
                return;
            }

            value = clamped(key, *read, limits.min, limits.max, intText);
        }

        void onBool(const std::string& key, bool& value, FieldApplies applies) override {
            const std::string* text = find(key, applies);

            if (text == nullptr) {
                return;
            }

            if (*text == "true" || *text == "false") {
                value = *text == "true";
                return;
            }

            warnings.push_back(key + ": \"" + *text + "\" is not true or false (kept " + boolText(value) + ")");
            ++refused;
        }

        void onChoice(const std::string& key, int& index, const char* const* names, int count, FieldApplies applies) override {
            const std::string* text = find(key, applies);

            if (text == nullptr) {
                return;
            }

            for (int candidate = 0; candidate < count; ++candidate) {
                if (*text == names[candidate]) {
                    index = candidate;
                    return;
                }
            }

            std::string message = key + ": \"" + *text + "\" is not one of";

            for (int candidate = 0; candidate < count; ++candidate) {
                message += candidate == 0 ? " " : ", ";
                message += names[candidate];
            }

            warnings.push_back(message + " (kept " + names[index] + ")");
            ++refused;
        }

    private:
        // The entry's text for a field, or null when there is none or it must be left alone.
        const std::string* find(const std::string& key, FieldApplies applies) {
            const auto found = values.find(key);

            if (found == values.end()) {
                return nullptr;
            }

            used.insert(key);

            if (applies == FieldApplies::AtStartup && startup == StartupFields::Keep) {
                return nullptr;
            }

            return &found->second;
        }

        template<typename Number, typename Text>
        Number clamped(const std::string& key, Number value, Number min, Number max, Text text) {
            if (value < min) {
                warnings.push_back(key + ": " + text(value) + " is below its limit, " + text(min) + " used");
                return min;
            }

            if (value > max) {
                warnings.push_back(key + ": " + text(value) + " is above its limit, " + text(max) + " used");
                return max;
            }

            return value;
        }

        StartupFields startup;
        std::map<std::string, std::string> values;
        std::set<std::string> used;
    };

    // Checks each value against its hard limits, and each slider range for being usable.
    class LimitCheckingVisitor final : public KeyedVisitor {
    public:
        std::vector<std::string> problems;

    protected:
        void onFloat(const std::string& key, float& value, const FloatLimits& limits) override {
            const float low = limits.lowestSliderValue();
            const float high = limits.highestSliderValue();

            if (!(value >= limits.min && value <= limits.max)) {
                problems.push_back(key + ": " + floatText(value) + " is outside its limits");
            }

            if (!std::isfinite(low) || !std::isfinite(high) || !(low < high) || low < limits.min || high > limits.max) {
                problems.push_back(key + ": its slider range must be finite, ordered and within its limits");
            }
        }

        void onInt(const std::string& key, int& value, const IntLimits& limits) override {
            const int low = limits.lowestSliderValue();
            const int high = limits.highestSliderValue();

            if (value < limits.min || value > limits.max) {
                problems.push_back(key + ": " + intText(value) + " is outside its limits");
            }

            const bool unbounded = low == std::numeric_limits<int>::min() || high == std::numeric_limits<int>::max();

            if (unbounded || !(low < high) || low < limits.min || high > limits.max) {
                problems.push_back(key + ": its slider range must be finite, ordered and within its limits");
            }
        }

        void onBool(const std::string&, bool&, FieldApplies) override {
        }

        void onChoice(const std::string& key, int& index, const char* const*, int count, FieldApplies) override {
            if (index < 0 || index >= count) {
                problems.push_back(key + ": " + intText(index) + " is not one of its choices");
            }
        }
    };

    // "0 to 1", "from 0", "up to 5" or nothing, for limits that may be missing (infinite, or an int's extreme).
    template<typename Number, typename Text>
    std::string rangeText(Number min, Number max, bool hasMin, bool hasMax, Text text) {
        if (hasMin && hasMax) {
            return text(min) + " to " + text(max);
        }

        if (hasMin) {
            return "from " + text(min);
        }

        return hasMax ? "up to " + text(max) : std::string{};
    }

    // A number field's kind: its word, its hard limits, its slider's range when it has its own, and its unit.
    template<typename Limits, typename Text>
    std::string numberKind(const char* word, const Limits& limits, bool hasMin, bool hasMax, Text text) {
        std::string kind = word;
        const std::string range = rangeText(limits.min, limits.max, hasMin, hasMax, text);

        if (!range.empty()) {
            kind += " " + range;
        }

        if (limits.sliderMin || limits.sliderMax) {
            kind += " (slider " + text(limits.lowestSliderValue()) + " to " + text(limits.highestSliderValue()) + ")";
        }

        if (limits.unit[0] != '\0') {
            kind += std::string{" "} + limits.unit;
        }

        return kind;
    }

    // Describes each field: its value, what it takes and when it applies.
    class DescribingVisitor final : public KeyedVisitor {
    public:
        std::vector<SettingDescription> descriptions;

    protected:
        void onFloat(const std::string& key, float& value, const FloatLimits& limits) override {
            const std::string kind = numberKind("number", limits, std::isfinite(limits.min), std::isfinite(limits.max), floatText);
            descriptions.push_back({key, floatText(value), kind, limits.applies == FieldApplies::AtStartup});
        }

        void onInt(const std::string& key, int& value, const IntLimits& limits) override {
            const bool hasMin = limits.min != std::numeric_limits<int>::min();
            const bool hasMax = limits.max != std::numeric_limits<int>::max();
            const std::string kind = numberKind("whole number", limits, hasMin, hasMax, intText);
            descriptions.push_back({key, intText(value), kind, limits.applies == FieldApplies::AtStartup});
        }

        void onBool(const std::string& key, bool& value, FieldApplies applies) override {
            descriptions.push_back({key, boolText(value), "true or false", applies == FieldApplies::AtStartup});
        }

        void onChoice(const std::string& key, int& index, const char* const* names, int count, FieldApplies applies) override {
            std::string kind = "one of";

            for (int candidate = 0; candidate < count; ++candidate) {
                kind += candidate == 0 ? " " : ", ";
                kind += names[candidate];
            }

            descriptions.push_back({key, names[index], kind, applies == FieldApplies::AtStartup});
        }
    };

    // The text with spaces and tabs trimmed from both ends.
    std::string_view trimmed(std::string_view text) {
        const std::size_t first = text.find_first_not_of(" \t\r");

        if (first == std::string_view::npos) {
            return {};
        }

        const std::size_t last = text.find_last_not_of(" \t\r");
        return text.substr(first, last - first + 1);
    }
}

std::vector<SettingEntry> captureSettings(const SettingsWalk& walk) {
    CapturingVisitor visitor;
    walk(visitor);
    return visitor.entries;
}

std::vector<std::string> applySettings(const std::vector<SettingEntry>& entries, const SettingsWalk& walk, StartupFields startup) {
    ApplyingVisitor visitor{entries, startup};
    walk(visitor);
    visitor.warnAboutUnknownKeys();
    return visitor.warnings;
}

/*
* A value is saved when its text differs from the default's. Comparing text rather than numbers is exact here, because
* each value has exactly one text (the shortest that reads back as it).
*/
std::string formatSettingsText(const std::vector<SettingEntry>& current, const std::vector<SettingEntry>& defaults,
                               const std::vector<SettingEntry>& kept) {
    std::map<std::string, std::string> defaultValues;

    for (const SettingEntry& entry : defaults) {
        defaultValues[entry.key] = entry.value;
    }

    std::string text =
        "# Settings changed from their defaults, saved by the debug UI (F5).\n"
        "# Delete a line to return that setting to its default.\n";

    for (const SettingEntry& entry : current) {
        const auto found = defaultValues.find(entry.key);

        if (found == defaultValues.end() || found->second != entry.value) {
            text += entry.key + " = " + entry.value + "\n";
        }
    }

    for (const SettingEntry& entry : kept) {
        text += entry.key + " = " + entry.value + "\n";
    }

    return text;
}

namespace {
    // A key's first part, its top-level section ("engine" of "engine.render.bloom.strength"), or nothing for a key with
    // no dot.
    std::string sectionOf(const std::string& key) {
        const std::size_t dot = key.find('.');
        return dot == std::string::npos ? std::string{} : key.substr(0, dot);
    }
}

SectionedSettings separateSections(const std::vector<SettingEntry>& entries, const SettingsWalk& walk) {
    std::set<std::string> walkedSections;

    for (const SettingEntry& field : captureSettings(walk)) {
        walkedSections.insert(sectionOf(field.key));
    }

    SectionedSettings sectioned;

    for (const SettingEntry& entry : entries) {
        const std::string section = sectionOf(entry.key);
        const bool own = section.empty() || walkedSections.count(section) > 0;
        (own ? sectioned.own : sectioned.others).push_back(entry);
    }

    return sectioned;
}

ParsedSettings parseSettingsText(std::string_view text) {
    ParsedSettings parsed;
    int lineNumber = 0;
    std::size_t lineStart = 0;

    while (lineStart < text.size()) {
        std::size_t lineEnd = text.find('\n', lineStart);

        if (lineEnd == std::string_view::npos) {
            lineEnd = text.size();
        }

        ++lineNumber;
        const std::string_view line = trimmed(text.substr(lineStart, lineEnd - lineStart));
        lineStart = lineEnd + 1;

        if (line.empty() || line.front() == '#') {
            continue;
        }

        const std::size_t equals = line.find('=');
        const std::string_view key = equals == std::string_view::npos ? std::string_view{} : trimmed(line.substr(0, equals));
        const std::string_view value = equals == std::string_view::npos ? std::string_view{} : trimmed(line.substr(equals + 1));

        if (key.empty() || value.empty()) {
            parsed.warnings.push_back("line " + std::to_string(lineNumber) + ": expected \"key = value\", got \"" + std::string(line) + "\"");
            continue;
        }

        parsed.entries.push_back({std::string(key), std::string(value)});
    }

    return parsed;
}

std::vector<SettingDescription> describeSettings(const SettingsWalk& walk) {
    DescribingVisitor visitor;
    walk(visitor);
    return visitor.descriptions;
}

/*
* The field is looked up first, so an unknown key and a start-up field are told apart from a bad value; then the value
* goes through the same visitor the settings file loads with, so a command and the file read values alike.
*/
SettingChange setSetting(const SettingsWalk& walk, const std::string& key, const std::string& value) {
    const std::vector<SettingDescription> fields = describeSettings(walk);
    const auto field = std::find_if(fields.begin(), fields.end(), [&](const SettingDescription& each) {
        return each.key == key;
    });

    if (field == fields.end()) {
        return {SettingOutcome::Unknown, {}, key + ": no such setting"};
    }

    if (field->atStartup) {
        return {SettingOutcome::AtStartupOnly, field->value, key + " is read only at start-up"};
    }

    ApplyingVisitor visitor{{{key, value}}, StartupFields::Keep};
    walk(visitor);

    const std::vector<SettingEntry> after = captureSettings(walk);
    const auto changed = std::find_if(after.begin(), after.end(), [&](const SettingEntry& entry) {
        return entry.key == key;
    });
    const std::string now = changed == after.end() ? std::string{} : changed->value;

    if (visitor.refused > 0) {
        return {SettingOutcome::Refused, now, visitor.warnings.front()};
    }

    if (!visitor.warnings.empty()) {
        return {SettingOutcome::Clamped, now, visitor.warnings.front()};
    }

    return {SettingOutcome::Set, now, {}};
}

std::vector<std::string> findLimitProblems(const SettingsWalk& walk) {
    LimitCheckingVisitor visitor;
    walk(visitor);
    return visitor.problems;
}

SettingsWalk engineSettings(EngineSettings& settings) {
    return [&settings](SettingsVisitor& visitor) {
        visitor.group("Engine", [&] {
            visit(settings, visitor);
        });
    };
}

SettingsWalk engineAndGameSettings(EngineSettings& settings, Game& game) {
    return [&settings, &game](SettingsVisitor& visitor) {
        engineSettings(settings)(visitor);
        visitor.group(game.title(), [&] {
            game.visitSettings(visitor);
        });
    };
}

std::optional<std::string> readSettingsFile(const char* path) {
    std::error_code error;

    if (!std::filesystem::exists(path, error)) {
        return std::nullopt;
    }

    std::ifstream file{path, std::ios::binary};

    if (!file) {
        throw std::runtime_error(std::string{"Could not read "} + path);
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

void writeSettingsFile(const char* path, const std::string& text) {
    std::ofstream file{path, std::ios::binary};
    file << text;

    if (!file) {
        throw std::runtime_error(std::string{"Could not write "} + path);
    }
}
