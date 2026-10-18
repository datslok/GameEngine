#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>

/*
* One description of every tunable value, walked by many visitors. Each settings struct has a visit() next to it that
* names its fields once (a label, the field itself, its limits and unit), and everything that needs the list of fields
* walks that one description: the debug UI draws a slider per field, the settings file saves and loads them, and the
* tests check every default against its limits. Adding a field to visit() is all it takes to get all of these.
*
* Labels are written for people ("Paper white"); the file key is made from them (settingKey: "paperWhite"), so a label
* is best named after its member.
*/

// When a change to a field takes effect. Start-up fields (the tick rate, GPU validation) are read once, so the debug UI
// shows them without letting them change; the settings file still loads them, before they are read.
enum class FieldApplies {
    EveryFrame,
    AtStartup
};

/*
* The limits of a number field. min and max are hard limits: values outside them are wrong (the settings file clamps
* to them, and the tests check every default lies within them). The slider covers the useful part, which may be much
* narrower (bloom strength is legal from 0 to 1, but useful only up to about 0.2); without a slider range it uses the
* hard limits, so a field with no finite hard limit must give one.
*/
struct FloatLimits {
    float min = -std::numeric_limits<float>::infinity();
    float max = std::numeric_limits<float>::infinity();
    std::optional<float> sliderMin = std::nullopt;
    std::optional<float> sliderMax = std::nullopt;
    const char* unit = "";
    FieldApplies applies = FieldApplies::EveryFrame;

    float lowestSliderValue() const {
        return sliderMin.value_or(min);
    }

    float highestSliderValue() const {
        return sliderMax.value_or(max);
    }
};

struct IntLimits {
    int min = std::numeric_limits<int>::min();
    int max = std::numeric_limits<int>::max();
    std::optional<int> sliderMin = std::nullopt;
    std::optional<int> sliderMax = std::nullopt;
    const char* unit = "";
    FieldApplies applies = FieldApplies::EveryFrame;

    int lowestSliderValue() const {
        return sliderMin.value_or(min);
    }

    int highestSliderValue() const {
        return sliderMax.value_or(max);
    }
};

/*
* A visitor sees each field in turn and may read or change it. The public functions are what visit() calls; the
* engine's types that are not float, int or bool (double, unsigned counts, enums) are converted here, once, so each
* visitor only handles the four basic kinds.
*/
class SettingsVisitor {
public:
    virtual ~SettingsVisitor() = default;

    void field(const char* label, float& value, const FloatLimits& limits) {
        visitFloat(label, value, limits);
    }

    void field(const char* label, int& value, const IntLimits& limits) {
        visitInt(label, value, limits);
    }

    void field(const char* label, bool& value, FieldApplies applies = FieldApplies::EveryFrame) {
        visitBool(label, value, applies);
    }

    // Seen as a float. The value is only written back when the visitor changed it, so a double the float cannot hold
    // exactly is not rounded by being looked at.
    void field(const char* label, double& value, const FloatLimits& limits) {
        const float original = static_cast<float>(value);
        float seen = original;
        visitFloat(label, seen, limits);

        if (seen != original) {
            value = static_cast<double>(seen);
        }
    }

    // Unsigned counts are seen as ints, so their limits must fit in an int.
    void field(const char* label, std::uint32_t& value, const IntLimits& limits) {
        fieldAsInt(label, value, limits);
    }

    void field(const char* label, std::uint64_t& value, const IntLimits& limits) {
        fieldAsInt(label, value, limits);
    }

    // An enum whose values are 0, 1, 2... in the order of their names.
    template<typename Enum, std::size_t Count>
    void choice(const char* label, Enum& value, const std::array<const char*, Count>& names, FieldApplies applies = FieldApplies::EveryFrame) {
        int index = static_cast<int>(value);
        visitChoice(label, index, names.data(), static_cast<int>(Count), applies);
        value = static_cast<Enum>(index);
    }

    // Fields that belong together (a struct inside a struct). A visitor may skip the contents (a closed tree node).
    void group(const char* label, const std::function<void()>& contents) {
        visitGroup(label, contents);
    }

protected:
    virtual void visitFloat(const char* label, float& value, const FloatLimits& limits) = 0;
    virtual void visitInt(const char* label, int& value, const IntLimits& limits) = 0;
    virtual void visitBool(const char* label, bool& value, FieldApplies applies) = 0;

    // index is 0..count-1 on the way in; a visitor that changes it keeps it in that range.
    virtual void visitChoice(const char* label, int& index, const char* const* names, int count, FieldApplies applies) = 0;

    virtual void visitGroup(const char* label, const std::function<void()>& contents) = 0;

private:
    template<typename Unsigned>
    void fieldAsInt(const char* label, Unsigned& value, const IntLimits& limits) {
        const int original = value > static_cast<Unsigned>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(value);
        int seen = original;
        visitInt(label, seen, limits);

        if (seen != original) {
            value = static_cast<Unsigned>(seen < 0 ? 0 : seen);
        }
    }
};

// Walks one tree of settings: usually a lambda calling visit() on a struct.
using SettingsWalk = std::function<void(SettingsVisitor&)>;

/*
* The settings file's name for a label: its words joined in lower camel case, letters and digits only ("Paper white"
* gives "paperWhite", "Max levels" gives "maxLevels"). Keys of nested fields are joined with dots by whoever walks them.
*/
std::string settingKey(const char* label);
