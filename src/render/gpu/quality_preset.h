#pragma once

#include "render/gpu/render_settings.h"

#include <array>
#include <optional>

/*
* Quality presets: one choice that sets every render setting whose cost is a trade between looks and speed, for GPUs
* of different strength. Ultra is the engine as tuned (its defaults). High is the same with the sun's cascades at 2048,
* whose shadow atlas takes 128 MB instead of 384, for typical GPUs; each step further down gives up shadow detail first
* (fewer lights with shadows, coarser shadow texels, a shorter reach for the sun's shadows in fewer, smaller cascades)
* and the widest bloom levels. Taste (tone mapping, exposure, the bloom's strength, HDR output) is never touched. An
* RTX preset joins these once ray traced global illumination exists (phase 6d). The numbers below Medium and Low are
* first guesses until GPU timings per pass (phase 6b) can measure them.
*/
enum class QualityPreset {
    Low,
    Medium,
    High,
    Ultra
};

// In the order of QualityPreset's values.
inline constexpr std::array<const char*, 4> qualityPresetNames{"Low", "Medium", "High", "Ultra"};

// Set the preset's values; all other settings are kept.
void applyQualityPreset(RenderSettings& settings, QualityPreset preset);

// The preset whose values the settings have, or nothing when one of them was changed by hand (custom settings).
std::optional<QualityPreset> matchingQualityPreset(const RenderSettings& settings);
