#pragma once

#include "render/gpu/render_settings.h"

#include <array>
#include <optional>

/*
* Quality presets: one choice that sets every render setting whose cost is a trade between looks and speed, for GPUs
* of different strength. Ultra is the engine as tuned (its defaults, with ray-traced reflections and mirrored lights on
* GPUs with ray queries). High is the same without the ray-traced features. Each step down gives up shadow detail, the
* widest bloom levels, light probe rays and probes, and ambient occlusion's slices and steps; each preset's values are
* the table in quality_preset.cpp (and docs/architecture/settings-and-commands.md). Taste (tone mapping, exposure, the
* bloom's strength, HDR output) is never touched. The values below High are first guesses until measured on weaker
* GPUs.
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
