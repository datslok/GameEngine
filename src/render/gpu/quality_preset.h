#pragma once

#include "render/gpu/render_settings.h"

#include <array>
#include <optional>

/*
* Quality presets: one choice that sets every render setting whose cost is a trade between looks and speed, for GPUs
* of different strength. High is the engine as tuned (its defaults: 4096 cascades, 128 light probe rays). Ultra is the
* same with 256 probe rays, near fast lights too. Each step down from High gives up shadow detail (fewer lights with
* shadows, coarser shadow texels, a shorter reach for the sun's shadows in fewer, smaller cascades: Medium's and Low's 2048
* cascades take 128 MB instead of 384), the widest bloom levels and probe rays (64, then 32, with more sharing between probes to make up: 0.3, then 0.5, against High's 0.1 and none on Ultra; the ray budget and the
* rest of global illumination stay as tuned). Taste (tone mapping, exposure, the bloom's strength, HDR output) is never
* touched. The numbers below High are first guesses until measured on weaker GPUs.
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
