#include "app/game_registry.h"
#include "core/settings_visitor.h"
#include "engine/engine_settings.h"
#include "engine/settings_file.h"
#include "games/demo/demo_game.h"
#include "render/gpu/bloom.h"
#include "render/gpu/tone_mapping.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {
    enum class Flavour {
        Plain,
        Salted,
        Spiced
    };

    constexpr std::array<const char*, 3> flavourNames{"Plain", "Salted", "Spiced"};

    // A small settings tree of every kind of field, standing in for the engine's.
    struct Inner {
        float speed = 2.5f;
        bool enabled = true;
    };

    struct Sample {
        float strength = 0.04f;
        int count = 3;
        std::uint32_t levels = 6;
        std::uint64_t frames = 240;
        double ticks = 120.0;
        Flavour flavour = Flavour::Salted;
        bool startupFlag = false;
        Inner inner;
    };

    void visit(Sample& sample, SettingsVisitor& visitor) {
        visitor.field("Strength", sample.strength, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.2f});
        visitor.field("Count", sample.count, {.min = 0, .max = 10});
        visitor.field("Levels", sample.levels, {.min = 1, .max = 8});
        visitor.field("Frames", sample.frames, {.min = 0, .max = 1000});
        visitor.field("Ticks", sample.ticks, {.min = 1.0f, .max = 1000.0f, .applies = FieldApplies::AtStartup});
        visitor.choice("Flavour", sample.flavour, flavourNames);
        visitor.field("Startup flag", sample.startupFlag, FieldApplies::AtStartup);
        visitor.group("Inner part", [&] {
            visitor.field("Speed", sample.inner.speed, {.min = 0.0f, .max = 10.0f});
            visitor.field("Enabled", sample.inner.enabled);
        });
    }

    SettingsWalk walkOf(Sample& sample) {
        return [&sample](SettingsVisitor& visitor) {
            visit(sample, visitor);
        };
    }

    std::string valueOf(const std::vector<SettingEntry>& entries, const std::string& key) {
        for (const SettingEntry& entry : entries) {
            if (entry.key == key) {
                return entry.value;
            }
        }

        assert(false && "no such key");
        return {};
    }

    bool containsText(const std::vector<std::string>& messages, const std::string& text) {
        return std::any_of(messages.begin(), messages.end(), [&](const std::string& message) {
            return message.find(text) != std::string::npos;
        });
    }

    // Keys are the label's words in lower camel case, so a label named after its member gives the member's name.
    void testSettingKeys() {
        assert(settingKey("Strength") == "strength");
        assert(settingKey("Paper white") == "paperWhite");
        assert(settingKey("Max levels") == "maxLevels");
        assert(settingKey("GPU validation") == "gpuValidation");
        assert(settingKey("Target FPS") == "targetFPS");
        assert(settingKey("Ticks per second") == "ticksPerSecond");
        assert(settingKey("Fade (seconds)") == "fadeSeconds");
    }

    // Every field appears once, in visiting order, under its group's key, with its value as text.
    void testCaptureListsEveryField() {
        Sample sample;
        const std::vector<SettingEntry> entries = captureSettings(walkOf(sample));

        std::vector<std::string> keys;
        for (const SettingEntry& entry : entries) {
            keys.push_back(entry.key);
        }

        const std::vector<std::string> expected{
            "strength", "count", "levels", "frames", "ticks", "flavour", "startupFlag", "innerPart.speed", "innerPart.enabled"};
        assert(keys == expected);

        // Floats are written as the shortest text that reads back as the same float, not 0.0399999991.
        assert(valueOf(entries, "strength") == "0.04");
        assert(valueOf(entries, "count") == "3");
        assert(valueOf(entries, "frames") == "240");
        assert(valueOf(entries, "ticks") == "120");
        assert(valueOf(entries, "flavour") == "Salted");
        assert(valueOf(entries, "startupFlag") == "false");
        assert(valueOf(entries, "innerPart.enabled") == "true");
    }

    // Looking at a double through its float view must not round it: only a changed value is written back.
    void testDoublesAreNotRoundedByLooking() {
        Sample sample;
        sample.ticks = 0.1;
        captureSettings(walkOf(sample));
        assert(sample.ticks == 0.1);
    }

    // Saving writes only the values that differ from the defaults, so a default improved later still reaches a
    // player who never changed it; loading the text back gives the same settings.
    void testSaveAndLoadRoundTrip() {
        Sample defaults;
        const std::vector<SettingEntry> defaultEntries = captureSettings(walkOf(defaults));

        Sample changed;
        changed.strength = 0.0625f;
        changed.levels = 4;
        changed.frames = 144;
        changed.flavour = Flavour::Spiced;
        changed.inner.enabled = false;

        const std::string text = formatSettingsText(captureSettings(walkOf(changed)), defaultEntries);
        assert(text.find("strength = 0.0625") != std::string::npos);
        assert(text.find("innerPart.enabled = false") != std::string::npos);
        assert(text.find("count") == std::string::npos);
        assert(text.find("innerPart.speed") == std::string::npos);

        const ParsedSettings parsed = parseSettingsText(text);
        assert(parsed.warnings.empty());

        Sample loaded;
        const std::vector<std::string> warnings = applySettings(parsed.entries, walkOf(loaded), StartupFields::Apply);
        assert(warnings.empty());
        assert(loaded.strength == changed.strength);
        assert(loaded.count == changed.count);
        assert(loaded.levels == changed.levels);
        assert(loaded.frames == changed.frames);
        assert(loaded.flavour == changed.flavour);
        assert(loaded.inner.enabled == changed.inner.enabled);
        assert(loaded.inner.speed == changed.inner.speed);
    }

    // Comments and blank lines are skipped; spaces round the key and value do not matter.
    void testParseSkipsCommentsAndSpaces() {
        const ParsedSettings parsed = parseSettingsText("# a comment\r\n\r\n   count=7   \r\ninnerPart.speed =  1.5\n");
        assert(parsed.warnings.empty());
        assert(parsed.entries.size() == 2);
        assert(parsed.entries[0].key == "count" && parsed.entries[0].value == "7");
        assert(parsed.entries[1].key == "innerPart.speed" && parsed.entries[1].value == "1.5");
    }

    // A line that is not "key = value" is reported with its line number and skipped; the rest still loads.
    void testMalformedLinesAreReported() {
        const ParsedSettings parsed = parseSettingsText("count = 7\nthis line is wrong\n= 3\ncount2 =\n");
        assert(parsed.entries.size() == 1);
        assert(parsed.warnings.size() == 3);
        assert(containsText(parsed.warnings, "line 2"));
        assert(containsText(parsed.warnings, "line 3"));
        assert(containsText(parsed.warnings, "line 4"));
    }

    // A hand-edited file must not break the game: values past the hard limits are clamped, values that do not read
    // as their kind keep the default, unknown keys (a renamed setting) are ignored, each with a warning.
    void testLoadingIsForgiving() {
        const ParsedSettings parsed = parseSettingsText(
            "strength = 5\n"
            "count = -2\n"
            "levels = 99\n"
            "frames = lots\n"
            "flavour = Sour\n"
            "innerPart.enabled = maybe\n"
            "oldName = 1\n");
        assert(parsed.warnings.empty());

        Sample sample;
        const std::vector<std::string> warnings = applySettings(parsed.entries, walkOf(sample), StartupFields::Apply);

        assert(sample.strength == 1.0f);
        assert(sample.count == 0);
        assert(sample.levels == 8);
        assert(sample.frames == 240);
        assert(sample.flavour == Flavour::Salted);
        assert(sample.inner.enabled);

        assert(warnings.size() == 7);
        assert(containsText(warnings, "strength"));
        assert(containsText(warnings, "frames"));
        assert(containsText(warnings, "Sour"));
        assert(containsText(warnings, "oldName"));
    }

    // Resetting to defaults while the game runs leaves start-up fields alone: they were read once already, and
    // changing them would make the struct disagree with what is running.
    void testStartupFieldsCanBeLeftAlone() {
        Sample sample;
        sample.ticks = 60.0;
        sample.startupFlag = true;
        sample.count = 9;

        Sample defaults;
        applySettings(captureSettings(walkOf(defaults)), walkOf(sample), StartupFields::Keep);

        assert(sample.ticks == 60.0);
        assert(sample.startupFlag);
        assert(sample.count == 3);
    }

    // Every field's value lies within its hard limits, and every slider has a finite range inside them.
    void testLimitProblemsAreFound() {
        Sample sample;
        assert(findLimitProblems(walkOf(sample)).empty());

        sample.strength = 2.0f;
        sample.count = 11;
        const std::vector<std::string> problems = findLimitProblems(walkOf(sample));
        assert(problems.size() == 2);
        assert(containsText(problems, "strength"));
        assert(containsText(problems, "count"));
    }

    void testUnboundedSliderIsAProblem() {
        float value = 1.0f;
        const SettingsWalk walk = [&](SettingsVisitor& visitor) {
            visitor.field("Unbounded", value, {.min = 0.0f});
        };

        assert(containsText(findLimitProblems(walk), "unbounded"));
    }

    // The engine's and the demo's real settings: every default within its limits, every slider finite, every key unique.
    void testEngineAndDemoDefaults() {
        EngineSettings settings;
        DemoGame game;
        const SettingsWalk walk = engineAndGameSettings(settings, game);

        const std::vector<std::string> problems = findLimitProblems(walk);
        assert(problems.empty());

        const std::vector<SettingEntry> entries = captureSettings(walk);
        std::set<std::string> keys;
        for (const SettingEntry& entry : entries) {
            assert(keys.insert(entry.key).second && "every key is unique");
        }

        // A sample of what must be there, under the engine's and the game's prefixes.
        assert(keys.count("engine.targetFPS") == 1);
        assert(keys.count("engine.render.bloom.strength") == 1);
        assert(keys.count("engine.render.toneMapping.curve") == 1);
        assert(keys.count("engine.render.display.paperWhite") == 1);
        assert(keys.count("demo.grenade.throwSpeed") == 1);
        assert(keys.count("demo.freeCamera.moveSpeed") == 1);
        assert(valueOf(entries, "engine.render.toneMapping.curve") == "AgX");

        // A round trip through the file text with nothing changed writes nothing but the header.
        const std::string text = formatSettingsText(entries, entries);
        assert(parseSettingsText(text).entries.empty());
    }

    // Every game's settings within their limits, under a section named after the game, distinct from the others'.
    void testEveryGameHasItsOwnSection() {
        std::set<std::string> sections;

        for (const GameEntry& entry : registeredGames()) {
            EngineSettings settings;
            const std::unique_ptr<Game> game = entry.make();
            const SettingsWalk walk = engineAndGameSettings(settings, *game);
            assert(findLimitProblems(walk).empty());

            const std::string section = settingKey(game->title());
            assert(sections.insert(section).second);

            bool any = false;
            for (const SettingEntry& setting : captureSettings(walk)) {
                const bool engine = setting.key.starts_with("engine.");
                assert(engine || setting.key.starts_with(section + "."));
                any = any || !engine;
            }
            assert(any);
        }

        assert(sections.count("lightLab") == 1);
    }

    /*
    * One file shared by every game: loading keeps the sections this game lacks apart (no warnings, nothing applied), and
    * saving writes them back unchanged after this game's own changes.
    */
    void testOtherGamesSectionsAreKept() {
        Sample sample;
        const SettingsWalk walk = [&sample](SettingsVisitor& visitor) {
            visitor.group("Engine", [&] { visit(sample, visitor); });
        };

        const std::vector<SettingEntry> saved{
            {"engine.strength", "0.1"}, {"lightLab.sun.solarHours", "9"}, {"stray", "1"}, {"demo.grenade.throwSpeed", "20"}
        };
        const SectionedSettings sectioned = separateSections(saved, walk);
        assert(sectioned.own.size() == 2);
        assert(sectioned.own[0].key == "engine.strength" && sectioned.own[1].key == "stray");
        assert(sectioned.others.size() == 2);
        assert(sectioned.others[0].key == "lightLab.sun.solarHours" && sectioned.others[1].key == "demo.grenade.throwSpeed");

        const std::vector<SettingEntry> defaults = captureSettings(walk);
        sample.count = 7;
        const std::string text = formatSettingsText(captureSettings(walk), defaults, sectioned.others);
        const ParsedSettings parsed = parseSettingsText(text);
        assert(parsed.entries.size() == 3);
        assert(valueOf(parsed.entries, "engine.count") == "7");
        assert(valueOf(parsed.entries, "lightLab.sun.solarHours") == "9");
        assert(valueOf(parsed.entries, "demo.grenade.throwSpeed") == "20");
    }

    // Each field described for people and commands: its key and value as saved, what it takes, and when it applies.
    void testSettingsAreDescribed() {
        Sample sample;
        const std::vector<SettingDescription> described = describeSettings(walkOf(sample));
        const std::vector<SettingEntry> captured = captureSettings(walkOf(sample));

        assert(described.size() == captured.size());
        for (std::size_t index = 0; index < described.size(); ++index) {
            assert(described[index].key == captured[index].key);
            assert(described[index].value == captured[index].value);
        }

        const auto find = [&](const std::string& key) {
            for (const SettingDescription& description : described) {
                if (description.key == key) {
                    return description;
                }
            }
            assert(false && "no such key");
            return SettingDescription{};
        };

        assert(find("strength").kind == "number 0 to 1 (slider 0 to 0.2)");
        assert(find("count").kind == "whole number 0 to 10");
        assert(find("flavour").kind == "one of Plain, Salted, Spiced");
        assert(find("innerPart.enabled").kind == "true or false");
        assert(find("ticks").atStartup);
        assert(find("startupFlag").atStartup);
        assert(!find("strength").atStartup);
    }

    // A single field set by key says what happened: set, clamped, refused, unknown or read only at start-up.
    void testOneSettingIsSet() {
        Sample sample;
        const SettingsWalk walk = walkOf(sample);

        const SettingChange set = setSetting(walk, "innerPart.speed", "4.5");
        assert(set.outcome == SettingOutcome::Set);
        assert(sample.inner.speed == 4.5f);
        assert(set.value == "4.5");

        const SettingChange clamped = setSetting(walk, "count", "50");
        assert(clamped.outcome == SettingOutcome::Clamped);
        assert(sample.count == 10);
        assert(clamped.value == "10");
        assert(!clamped.message.empty());

        const SettingChange refused = setSetting(walk, "count", "many");
        assert(refused.outcome == SettingOutcome::Refused);
        assert(sample.count == 10);
        assert(!refused.message.empty());

        assert(setSetting(walk, "flavour", "Sour").outcome == SettingOutcome::Refused);
        assert(sample.flavour == Flavour::Salted);

        assert(setSetting(walk, "nothing", "1").outcome == SettingOutcome::Unknown);
        assert(setSetting(walk, "innerPart", "1").outcome == SettingOutcome::Unknown);

        const SettingChange startup = setSetting(walk, "ticks", "60");
        assert(startup.outcome == SettingOutcome::AtStartupOnly);
        assert(sample.ticks == 120.0);
    }

    // The hard limits agree with the renderer's own checks: their ends are accepted.
    void testLimitsAgreeWithValidation() {
        BloomSettings bloom;
        bloom.strength = maxBloomStrength;
        bloom.maxLevels = BLOOM_LEVEL_LIMIT;
        validateBloom(bloom);

        bloom.strength = 0.0f;
        bloom.maxLevels = 1;
        validateBloom(bloom);

        ToneMappingSettings toneMapping;
        toneMapping.exposure = 0.0f;
        validateToneMapping(toneMapping, sdrPeakBrightness);
    }
}

void testSettingsFile() {
    testSettingKeys();
    testCaptureListsEveryField();
    testDoublesAreNotRoundedByLooking();
    testSaveAndLoadRoundTrip();
    testParseSkipsCommentsAndSpaces();
    testMalformedLinesAreReported();
    testLoadingIsForgiving();
    testStartupFieldsCanBeLeftAlone();
    testLimitProblemsAreFound();
    testUnboundedSliderIsAProblem();
    testEngineAndDemoDefaults();
    testEveryGameHasItsOwnSection();
    testOtherGamesSectionsAreKept();
    testLimitsAgreeWithValidation();
    testSettingsAreDescribed();
    testOneSettingIsSet();
}
