#include "render/gpu/quality_preset.h"
#include "render/gpu/render_settings.h"
#include "render/gpu/shadow_map.h"

#include <array>
#include <cassert>
#include <optional>

namespace {
    // Ultra is the engine as it is tuned: its defaults (the user's choice on Oct 23 2026; it was High).
    void testUltraIsTheDefaults() {
        const RenderSettings defaults;
        assert(matchingQualityPreset(defaults) == QualityPreset::Ultra);

        RenderSettings settings;
        settings.shadows.texelTarget = 0.5f;
        settings.globalIllumination.raysPerProbe = 200;
        applyQualityPreset(settings, QualityPreset::Ultra);
        assert(settings.shadows.texelTarget == defaults.shadows.texelTarget);
        assert(settings.globalIllumination.raysPerProbe == defaults.globalIllumination.raysPerProbe);
    }

    // The light probes' rays per preset, the same for probes near fast lights: 32, 64, then 256 on High and Ultra (the
    // defaults).
    void testPresetsSetTheProbeRays() {
        const std::array<int, 4> rays{32, 64, 256, 256};

        for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High, QualityPreset::Ultra}) {
            RenderSettings settings;
            applyQualityPreset(settings, preset);
            const int expected = rays[static_cast<std::size_t>(preset)];
            assert(settings.globalIllumination.raysPerProbe == expected);
            assert(settings.globalIllumination.fastRaysPerProbe == expected);
        }

        assert(RenderSettings{}.globalIllumination.raysPerProbe == 256 && RenderSettings{}.globalIllumination.fastRaysPerProbe == 256);
    }

    // Reflections per preset: off on Low, fewer steps and a smaller probe on Medium, the defaults' probe on Ultra.
    void testPresetsSetReflections() {
        const std::array<bool, 4> enabled{false, true, true, true};
        const std::array<int, 4> steps{32, 32, 64, 64};
        const std::array<int, 4> probeSizes{64, 64, 128, 256};

        for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High, QualityPreset::Ultra}) {
            RenderSettings settings;
            settings.reflections.enabled = !enabled[static_cast<std::size_t>(preset)];
            applyQualityPreset(settings, preset);
            assert(settings.reflections.enabled == enabled[static_cast<std::size_t>(preset)]);
            assert(settings.reflections.maxSteps == steps[static_cast<std::size_t>(preset)]);
            assert(settings.reflections.probeSize == probeSizes[static_cast<std::size_t>(preset)]);
            assert(matchingQualityPreset(settings) == preset);
        }

        assert(RenderSettings{}.reflections.enabled && RenderSettings{}.reflections.maxSteps == 64 && RenderSettings{}.reflections.probeSize == 256);
    }

    // The ray-traced features (on GPUs with ray queries; ignored elsewhere) per preset: Ultra only, as the defaults.
    void testPresetsSetRayTracedFeatures() {
        for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High, QualityPreset::Ultra}) {
            const bool on = preset == QualityPreset::Ultra;
            RenderSettings settings;
            settings.reflections.rayTraced = !on;
            settings.mirroredLights.enabled = !on;
            applyQualityPreset(settings, preset);
            assert(settings.reflections.rayTraced == on && settings.mirroredLights.enabled == on);
            assert(matchingQualityPreset(settings) == preset);
        }

        assert(RenderSettings{}.reflections.rayTraced && RenderSettings{}.mirroredLights.enabled);
    }

    // Ambient occlusion per preset: off on Low, fewer slices and steps on Medium and High, the defaults on Ultra.
    void testPresetsSetAmbientOcclusion() {
        const std::array<bool, 4> enabled{false, true, true, true};
        const std::array<int, 4> slices{2, 2, 2, 3};
        const std::array<int, 4> steps{3, 3, 4, 6};

        for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High, QualityPreset::Ultra}) {
            RenderSettings settings;
            settings.ambientOcclusion.enabled = !enabled[static_cast<std::size_t>(preset)];
            applyQualityPreset(settings, preset);
            assert(settings.ambientOcclusion.enabled == enabled[static_cast<std::size_t>(preset)]);
            assert(settings.ambientOcclusion.slices == slices[static_cast<std::size_t>(preset)]);
            assert(settings.ambientOcclusion.steps == steps[static_cast<std::size_t>(preset)]);
            assert(matchingQualityPreset(settings) == preset);
        }

        const AmbientOcclusionSettings defaults = RenderSettings{}.ambientOcclusion;
        assert(defaults.enabled && defaults.slices == 3 && defaults.steps == 6);
    }

    // Sharing between probes per preset: more where there are fewer rays, none on Ultra, the default.
    void testPresetsSetProbeSharing() {
        const std::array<float, 4> sharing{0.5f, 0.3f, 0.1f, 0.0f};

        for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High, QualityPreset::Ultra}) {
            RenderSettings settings;
            applyQualityPreset(settings, preset);
            assert(settings.globalIllumination.probeSharing == sharing[static_cast<std::size_t>(preset)]);
            assert(matchingQualityPreset(settings) == preset);
        }

        assert(RenderSettings{}.globalIllumination.probeSharing == 0.0f);

        // Fast lights' sharing is the same on every preset.
        RenderSettings fast;
        fast.globalIllumination.fastProbeSharing = 0.25f;
        applyQualityPreset(fast, QualityPreset::Low);
        assert(fast.globalIllumination.fastProbeSharing == 0.25f);
    }

    /*
    * The light probes' cascades per preset: Ultra (the defaults) and High six from half a metre to 16 m, a densest of 31 x 17 x 31
    * and five of 21 x 13 x 21, Medium five (to 8 m), a densest of 25 x 15 x 25 and four of 17 x 11 x 17,
    * Low four from a metre (to 8 m), a densest of 19 x 11 x 19 and three of 15 x 9 x 15; each lower one holds fewer probes.
    */
    void testPresetsSetTheProbeCascades() {
        struct Cascades {
            int cascades;
            float spacing;
            int densestAcross;
            int densestHigh;
            int across;
            int high;
        };

        const std::array<Cascades, 4> expected{Cascades{4, 1.0f, 19, 11, 15, 9}, Cascades{5, 0.5f, 25, 15, 17, 11}, Cascades{6, 0.5f, 31, 17, 21, 13},
                                               Cascades{6, 0.5f, 31, 17, 21, 13}};
        int probesAbove = 0;

        for (QualityPreset preset : {QualityPreset::Ultra, QualityPreset::High, QualityPreset::Medium, QualityPreset::Low}) {
            RenderSettings settings;
            applyQualityPreset(settings, preset);
            const GlobalIlluminationSettings& probes = settings.globalIllumination;
            const Cascades& wanted = expected[static_cast<std::size_t>(preset)];
            assert(probes.probeCascades == wanted.cascades && probes.probeSpacing == wanted.spacing);
            assert(probes.densestProbesAcross == wanted.densestAcross && probes.densestProbesHigh == wanted.densestHigh);
            assert(probes.cascadeProbesAcross == wanted.across && probes.cascadeProbesHigh == wanted.high);

            const int held = probes.densestProbesAcross * probes.densestProbesAcross * probes.densestProbesHigh +
                             (probes.probeCascades - 1) * probes.cascadeProbesAcross * probes.cascadeProbesAcross * probes.cascadeProbesHigh;
            assert(held <= maxProbesPerVolume);
            assert(preset == QualityPreset::Ultra || preset == QualityPreset::High || held < probesAbove);
            probesAbove = held;
        }

        const GlobalIlluminationSettings defaults;
        assert(defaults.probeCascades == 6 && defaults.probeSpacing == 0.5f && defaults.cascadeProbesAcross == 21 && defaults.cascadeProbesHigh == 13);
        assert(defaults.densestProbesAcross == 31 && defaults.densestProbesHigh == 17);
    }

    // Each step down from High costs less on every axis a preset sets: fewer shadowed lights, coarser shadow texels,
    // shorter sun shadows, fewer bloom levels and probe rays. None ever asks for more than the engine's limits.
    void testLowerPresetsCostLess() {
        RenderSettings low;
        RenderSettings medium;
        RenderSettings high;
        applyQualityPreset(low, QualityPreset::Low);
        applyQualityPreset(medium, QualityPreset::Medium);
        applyQualityPreset(high, QualityPreset::High);

        assert(low.lights.shadowedPoints < medium.lights.shadowedPoints);
        assert(medium.lights.shadowedPoints < high.lights.shadowedPoints);
        assert(low.lights.shadowedSpots <= medium.lights.shadowedSpots);
        assert(medium.lights.shadowedSpots <= high.lights.shadowedSpots);
        assert(high.lights.shadowedPoints <= maxShadowedPointLights);
        assert(high.lights.shadowedSpots <= maxShadowedSpotLights);

        assert(low.shadows.texelTarget > medium.shadows.texelTarget);
        assert(medium.shadows.texelTarget > high.shadows.texelTarget);
        assert(low.shadows.spotTexelTarget > medium.shadows.spotTexelTarget);
        assert(medium.shadows.spotTexelTarget > high.shadows.spotTexelTarget);
        assert(low.shadows.directionalDistance <= medium.shadows.directionalDistance);
        assert(medium.shadows.directionalDistance <= high.shadows.directionalDistance);
        assert(low.shadows.cascadeCount <= medium.shadows.cascadeCount);
        assert(medium.shadows.cascadeCount <= high.shadows.cascadeCount);
        assert(high.shadows.cascadeCount <= maxShadowCascades);
        assert(cascadeTilePixels(low.shadows.cascadeResolution) <= cascadeTilePixels(medium.shadows.cascadeResolution));
        assert(cascadeTilePixels(medium.shadows.cascadeResolution) <= cascadeTilePixels(high.shadows.cascadeResolution));

        assert(low.bloom.maxLevels < medium.bloom.maxLevels);
        assert(medium.bloom.maxLevels < high.bloom.maxLevels);

        assert(low.globalIllumination.raysPerProbe < medium.globalIllumination.raysPerProbe);
        assert(medium.globalIllumination.raysPerProbe < high.globalIllumination.raysPerProbe);
        assert(low.globalIllumination.fastRaysPerProbe < medium.globalIllumination.fastRaysPerProbe);
        assert(medium.globalIllumination.fastRaysPerProbe < high.globalIllumination.fastRaysPerProbe);
    }

    // A preset sets quality, never taste: tone mapping, HDR output and the bloom's look stay as the player set them.
    void testPresetsKeepTasteSettings() {
        RenderSettings settings;
        settings.toneMapping.exposure = 2.0f;
        settings.display.paperWhiteNits = 250.0f;
        settings.bloom.strength = 0.08f;
        settings.lights.fadeSeconds = 0.5f;

        applyQualityPreset(settings, QualityPreset::Low);

        assert(settings.toneMapping.exposure == 2.0f);
        assert(settings.display.paperWhiteNits == 250.0f);
        assert(settings.bloom.strength == 0.08f);
        assert(settings.lights.fadeSeconds == 0.5f);
        assert(matchingQualityPreset(settings) == QualityPreset::Low);
    }

    // After a slider moves one of a preset's values, no preset matches: the settings are custom.
    void testChangedValuesAreCustom() {
        RenderSettings settings;
        applyQualityPreset(settings, QualityPreset::Medium);
        settings.bloom.maxLevels += 1;
        assert(matchingQualityPreset(settings) == std::nullopt);
    }

    // High is Ultra without the ray-traced reflections and mirrored lights, with a little probe sharing, a reflection
    // probe half as sharp and fewer ambient occlusion slices and steps, nothing else: the same 256 probe rays, the same
    // 4096 cascades and the rest as tuned.
    void testHighIsUltraLighter() {
        RenderSettings high;
        applyQualityPreset(high, QualityPreset::High);
        assert(high.shadows.cascadeResolution == RenderSettings{}.shadows.cascadeResolution);
        assert(high.globalIllumination.raysPerProbe == RenderSettings{}.globalIllumination.raysPerProbe);
        assert(matchingQualityPreset(high) == QualityPreset::High);

        high.reflections.rayTraced = RenderSettings{}.reflections.rayTraced;
        high.mirroredLights.enabled = RenderSettings{}.mirroredLights.enabled;
        high.globalIllumination.probeSharing = RenderSettings{}.globalIllumination.probeSharing;
        high.reflections.probeSize = RenderSettings{}.reflections.probeSize;
        high.ambientOcclusion.slices = RenderSettings{}.ambientOcclusion.slices;
        high.ambientOcclusion.steps = RenderSettings{}.ambientOcclusion.steps;
        assert(matchingQualityPreset(high) == QualityPreset::Ultra);
    }

    /*
    * Low's sun cascades are three of 2048, like Medium's: 1024 saved no memory (the atlas is 128 MB for any cascade up to
    * 2048) and, like two cascades, left the light lab's 30 cm walls with ragged sunlight along their edges. Low still
    * reaches half as far.
    */
    void testLowKeepsSharpEnoughCascades() {
        RenderSettings low;
        RenderSettings medium;
        applyQualityPreset(low, QualityPreset::Low);
        applyQualityPreset(medium, QualityPreset::Medium);
        assert(low.shadows.cascadeResolution == CascadeResolution::Pixels2048);
        assert(low.shadows.cascadeCount == 3);
        assert(low.shadows.directionalDistance < medium.shadows.directionalDistance);
    }

    void testPresetNames() {
        assert(qualityPresetNames.size() == static_cast<std::size_t>(QualityPreset::Ultra) + 1);
    }
}

void testQualityPreset() {
    testHighIsUltraLighter();
    testUltraIsTheDefaults();
    testPresetsSetTheProbeRays();
    testPresetsSetProbeSharing();
    testPresetsSetReflections();
    testPresetsSetAmbientOcclusion();
    testPresetsSetRayTracedFeatures();
    testPresetsSetTheProbeCascades();
    testLowerPresetsCostLess();
    testPresetsKeepTasteSettings();
    testChangedValuesAreCustom();
    testLowKeepsSharpEnoughCascades();
    testPresetNames();
}
