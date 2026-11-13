#include "core/command_registry.h"
#include "engine/engine_commands.h"
#include "engine/engine_settings.h"
#include "engine/settings_file.h"
#include "render/gpu/quality_preset.h"
#include "render/gpu/typical_gpu.h"

#include <algorithm>
#include <cassert>
#include <set>
#include <string>
#include <vector>

namespace {
    bool contains(const std::string& text, const std::string& part) {
        return text.find(part) != std::string::npos;
    }

    // Every render setting as the settings file would save it, to compare two whole sets of settings.
    std::vector<SettingEntry> captured(RenderSettings& settings) {
        return captureSettings([&](SettingsVisitor& visitor) { visit(settings, visitor); });
    }

    bool sameSettings(RenderSettings& a, RenderSettings& b) {
        const std::vector<SettingEntry> first = captured(a);
        const std::vector<SettingEntry> second = captured(b);
        return std::equal(first.begin(), first.end(), second.begin(), second.end(), [](const SettingEntry& x, const SettingEntry& y) {
            return x.key == y.key && x.value == y.value;
        });
    }

    const UpscalerSupport everything{.fsr = true, .dlss = true};
    const UpscalerSupport fsrOnly{.fsr = true, .dlss = false};
    const UpscalerSupport neither{};

    // The upscaler a typical GPU runs: DLSS where it can, else FSR, else the engine's own TAA.
    void testTheUpscalerFallsBack() {
        assert(typicalUpscaler(everything) == Upscaler::Dlss);
        assert(typicalUpscaler(UpscalerSupport{.fsr = false, .dlss = true}) == Upscaler::Dlss);
        assert(typicalUpscaler(fsrOnly) == Upscaler::Fsr);
        assert(typicalUpscaler(neither) == Upscaler::Taa);
    }

    /*
    * The typical GPU setup sets exactly the High preset, ray tracing off and the upscaler at Quality (a render scale of
    * 0.67), and nothing else: taste and everything the preset leaves alone stay as they were.
    */
    void testItSetsOnlyItsSettings() {
        for (const UpscalerSupport support : {everything, fsrOnly, neither}) {
            RenderSettings settings;
            settings.rayTracing = true;
            settings.bloom.strength = 0.07f;
            settings.upscaling.sharpness = 0.3f;

            RenderSettings expected = settings;
            applyQualityPreset(expected, QualityPreset::High);
            expected.rayTracing = false;
            expected.upscaling.upscaler = typicalUpscaler(support);
            expected.upscaling.renderScale = upscalingModeScale(UpscalingMode::Quality);

            applyTypicalGpu(settings, support);
            assert(sameSettings(settings, expected));
            assert(matchingQualityPreset(settings) == QualityPreset::High && !settings.rayTracing);
            assert(matchingUpscalingMode(settings.upscaling.renderScale) == UpscalingMode::Quality);
            assert(settings.bloom.strength == 0.07f && settings.upscaling.sharpness == 0.3f);
        }
    }

    /*
    * The switch remembers the settings it found: off gives them back exactly, whatever was changed while it was on,
    * and turning it on again while on keeps the first ones. Off while off does nothing.
    */
    void testTheSwitchRestores() {
        RenderSettings settings;
        settings.rayTracing = true;
        settings.bloom.strength = 0.05f;
        RenderSettings original = settings;

        TypicalGpuView view;
        assert(!view.isOn());
        view.turnOff(settings);
        assert(sameSettings(settings, original));

        view.turnOn(settings, everything);
        assert(view.isOn() && !settings.rayTracing);
        settings.bloom.strength = 0.2f;
        view.turnOn(settings, fsrOnly);
        assert(view.isOn() && settings.upscaling.upscaler == Upscaler::Fsr);

        view.turnOff(settings);
        assert(!view.isOn() && sameSettings(settings, original));
    }

    // A registry with the settings and render commands over one EngineSettings, its GPU as the tests say.
    struct RenderCommandSetup {
        EngineSettings settings;
        TypicalGpuView view;
        CommandRegistry registry;
        bool rayQueries = false;

        RenderCommandSetup() {
            EngineSettings defaults;
            addSettingsCommands(registry, engineSettings(settings), captureSettings(engineSettings(defaults)), [this] { return rayQueries; });
            addRenderCommands(registry, settings.render, view, [] { return fsrOnly; });
        }
    };

    // preset: which preset the settings match (or custom), and choosing one by name, in any case.
    void testPresetCommand() {
        RenderCommandSetup setup;
        assert(setup.registry.execute("preset").text == "Ultra");

        const CommandReply high = setup.registry.execute("preset High");
        assert(high.ok && matchingQualityPreset(setup.settings.render) == QualityPreset::High);
        assert(setup.registry.execute("preset medium").ok && matchingQualityPreset(setup.settings.render) == QualityPreset::Medium);
        assert(setup.registry.execute("preset").text == "Medium");

        setup.settings.render.shadows.texelTarget = 0.123f;
        assert(setup.registry.execute("preset").text == "Custom");

        const CommandReply wrong = setup.registry.execute("preset Epic");
        assert(!wrong.ok && contains(wrong.text, "Low") && contains(wrong.text, "Ultra"));
        assert(!setup.registry.execute("preset High Low").ok);
    }

    // typical-gpu: on, off, and alone says which, naming the upscaler it chose.
    void testTypicalGpuCommand() {
        RenderCommandSetup setup;
        setup.settings.render.rayTracing = true;
        assert(contains(setup.registry.execute("typical-gpu").text, "off"));

        const CommandReply on = setup.registry.execute("typical-gpu on");
        assert(on.ok && contains(on.text, "FSR") && contains(on.text, "High") && setup.view.isOn());
        assert(!setup.settings.render.rayTracing && setup.settings.render.upscaling.upscaler == Upscaler::Fsr);
        assert(contains(setup.registry.execute("typical-gpu").text, "on"));

        assert(setup.registry.execute("typical-gpu off").ok && !setup.view.isOn() && setup.settings.render.rayTracing);
        assert(!setup.registry.execute("typical-gpu maybe").ok);
    }

    /*
    * Settings that only work with hardware ray tracing are tagged in their visit(): ray tracing itself, its debug view,
    * ray-traced reflections, the mirrored lights' search (not the image lights, which need none) and every light probe
    * setting. Nothing else is. set still takes them, saying they do nothing on a GPU without ray queries.
    */
    void testRayTracedSettingsAreTagged() {
        EngineSettings settings;
        std::set<std::string> tagged;

        for (const SettingDescription& field : describeSettings(engineSettings(settings))) {
            if (field.needsRayQueries) {
                tagged.insert(field.key);
            }
        }

        for (const char* key : {"engine.render.passes.rayTracing", "engine.render.passes.rayDebugView", "engine.render.reflections.rayTraced",
                                "engine.render.reflections.mirroredLights", "engine.render.reflections.mirroredLightBounce",
                                "engine.render.reflections.mirrorRoughness", "engine.render.globalIllumination.enabled",
                                "engine.render.globalIllumination.raysPerProbe"}) {
            assert(tagged.contains(key));
        }

        for (const std::string& key : tagged) {
            const bool known = key.starts_with("engine.render.globalIllumination.") || key == "engine.render.passes.rayTracing" ||
                               key == "engine.render.passes.rayDebugView" || key == "engine.render.reflections.rayTraced" ||
                               key == "engine.render.reflections.mirroredLights" || key == "engine.render.reflections.mirroredLightBounce" ||
                               key == "engine.render.reflections.mirrorRoughness";
            assert(known);
        }

        assert(!tagged.contains("engine.render.reflections.imageLights") && !tagged.contains("engine.render.bloom.strength"));

        RenderCommandSetup setup;
        const CommandReply withoutRays = setup.registry.execute("set engine.render.passes.rayTracing true");
        assert(withoutRays.ok && setup.settings.render.rayTracing && contains(withoutRays.text, "no effect"));
        assert(!contains(setup.registry.execute("set engine.render.bloom.strength 0.1").text, "no effect"));

        setup.rayQueries = true;
        assert(!contains(setup.registry.execute("set engine.render.passes.rayTracing false").text, "no effect"));
    }
}

void testTypicalGpu() {
    testTheUpscalerFallsBack();
    testItSetsOnlyItsSettings();
    testTheSwitchRestores();
    testPresetCommand();
    testTypicalGpuCommand();
    testRayTracedSettingsAreTagged();
}
