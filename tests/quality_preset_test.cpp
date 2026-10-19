#include "render/gpu/quality_preset.h"
#include "render/gpu/render_settings.h"
#include "render/gpu/shadow_map.h"

#include <cassert>
#include <optional>

namespace {
    // Ultra is the engine as it is tuned: its defaults.
    void testUltraIsTheDefaults() {
        const RenderSettings defaults;
        assert(matchingQualityPreset(defaults) == QualityPreset::Ultra);

        RenderSettings settings;
        settings.shadows.texelTarget = 0.5f;
        applyQualityPreset(settings, QualityPreset::Ultra);
        assert(settings.shadows.texelTarget == defaults.shadows.texelTarget);
    }

    // Each step down costs less on every axis a preset sets: fewer shadowed lights, coarser shadow texels, shorter
    // sun shadows and fewer bloom levels. None ever asks for more than the engine's limits.
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
        assert(cascadeTilePixels(high.shadows.cascadeResolution) < cascadeTilePixels(RenderSettings{}.shadows.cascadeResolution));

        assert(low.bloom.maxLevels < medium.bloom.maxLevels);
        assert(medium.bloom.maxLevels < high.bloom.maxLevels);
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

    // High is Ultra with the sun's cascades at 2048, in the usual atlas (128 MB instead of 384), for typical GPUs.
    void testHighIsUltraWithSmallerCascades() {
        RenderSettings high;
        applyQualityPreset(high, QualityPreset::High);
        assert(high.shadows.cascadeResolution == CascadeResolution::Pixels2048);
        assert(matchingQualityPreset(high) == QualityPreset::High);

        high.shadows.cascadeResolution = RenderSettings{}.shadows.cascadeResolution;
        assert(matchingQualityPreset(high) == QualityPreset::Ultra);
    }

    void testPresetNames() {
        assert(qualityPresetNames.size() == static_cast<std::size_t>(QualityPreset::Ultra) + 1);
    }
}

void testQualityPreset() {
    testHighIsUltraWithSmallerCascades();
    testUltraIsTheDefaults();
    testLowerPresetsCostLess();
    testPresetsKeepTasteSettings();
    testChangedValuesAreCustom();
    testPresetNames();
}
