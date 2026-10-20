#include "render/gpu/quality_preset.h"
#include "render/gpu/render_settings.h"
#include "render/gpu/shadow_map.h"

#include <array>
#include <cassert>
#include <optional>

namespace {
    // High is the engine as it is tuned: its defaults.
    void testHighIsTheDefaults() {
        const RenderSettings defaults;
        assert(matchingQualityPreset(defaults) == QualityPreset::High);

        RenderSettings settings;
        settings.shadows.texelTarget = 0.5f;
        settings.globalIllumination.raysPerProbe = 200;
        applyQualityPreset(settings, QualityPreset::High);
        assert(settings.shadows.texelTarget == defaults.shadows.texelTarget);
        assert(settings.globalIllumination.raysPerProbe == defaults.globalIllumination.raysPerProbe);
    }

    // The light probes' rays per preset, the same for probes near fast lights: 32, 64, 128 (the defaults) and 256.
    void testPresetsSetTheProbeRays() {
        const std::array<int, 4> rays{32, 64, 128, 256};

        for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High, QualityPreset::Ultra}) {
            RenderSettings settings;
            applyQualityPreset(settings, preset);
            const int expected = rays[static_cast<std::size_t>(preset)];
            assert(settings.globalIllumination.raysPerProbe == expected);
            assert(settings.globalIllumination.fastRaysPerProbe == expected);
        }

        assert(RenderSettings{}.globalIllumination.raysPerProbe == 128 && RenderSettings{}.globalIllumination.fastRaysPerProbe == 128);
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

    // Ultra is High with more light probe rays, nothing else: the same 4096 cascades and everything else as tuned.
    void testUltraIsHighWithMoreProbeRays() {
        RenderSettings ultra;
        applyQualityPreset(ultra, QualityPreset::Ultra);
        assert(ultra.shadows.cascadeResolution == RenderSettings{}.shadows.cascadeResolution);
        assert(matchingQualityPreset(ultra) == QualityPreset::Ultra);

        ultra.globalIllumination.raysPerProbe = RenderSettings{}.globalIllumination.raysPerProbe;
        ultra.globalIllumination.fastRaysPerProbe = RenderSettings{}.globalIllumination.fastRaysPerProbe;
        assert(matchingQualityPreset(ultra) == QualityPreset::High);
    }

    void testPresetNames() {
        assert(qualityPresetNames.size() == static_cast<std::size_t>(QualityPreset::Ultra) + 1);
    }
}

void testQualityPreset() {
    testUltraIsHighWithMoreProbeRays();
    testHighIsTheDefaults();
    testPresetsSetTheProbeRays();
    testLowerPresetsCostLess();
    testPresetsKeepTasteSettings();
    testChangedValuesAreCustom();
    testPresetNames();
}
