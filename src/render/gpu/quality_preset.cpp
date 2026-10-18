#include "render/gpu/quality_preset.h"

namespace {
    // Everything a preset sets, in one place, so applying and matching cannot disagree.
    struct QualityValues {
        int shadowedPoints = 0;
        int shadowedSpots = 0;
        float texelTarget = 0.0f;
        float spotTexelTarget = 0.0f;
        float directionalDistance = 0.0f;
        std::uint32_t bloomLevels = 0;

        bool operator==(const QualityValues&) const = default;
    };

    QualityValues valuesOf(const RenderSettings& settings) {
        return {
            .shadowedPoints = settings.lights.shadowedPoints,
            .shadowedSpots = settings.lights.shadowedSpots,
            .texelTarget = settings.shadows.texelTarget,
            .spotTexelTarget = settings.shadows.spotTexelTarget,
            .directionalDistance = settings.shadows.directionalDistance,
            .bloomLevels = settings.bloom.maxLevels
        };
    }

    // High is read from the defaults, so retuning a default retunes High with it.
    QualityValues presetValues(QualityPreset preset) {
        switch (preset) {
            case QualityPreset::Low:
                return {.shadowedPoints = 8, .shadowedSpots = 4, .texelTarget = 0.1f, .spotTexelTarget = 0.05f,
                        .directionalDistance = 20.0f, .bloomLevels = 4};
            case QualityPreset::Medium:
                return {.shadowedPoints = 24, .shadowedSpots = 8, .texelTarget = 0.06f, .spotTexelTarget = 0.03f,
                        .directionalDistance = 30.0f, .bloomLevels = 5};
            case QualityPreset::High:
                break;
        }

        return valuesOf(RenderSettings{});
    }
}

void applyQualityPreset(RenderSettings& settings, QualityPreset preset) {
    const QualityValues values = presetValues(preset);
    settings.lights.shadowedPoints = values.shadowedPoints;
    settings.lights.shadowedSpots = values.shadowedSpots;
    settings.shadows.texelTarget = values.texelTarget;
    settings.shadows.spotTexelTarget = values.spotTexelTarget;
    settings.shadows.directionalDistance = values.directionalDistance;
    settings.bloom.maxLevels = values.bloomLevels;
}

std::optional<QualityPreset> matchingQualityPreset(const RenderSettings& settings) {
    const QualityValues current = valuesOf(settings);

    for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High}) {
        if (presetValues(preset) == current) {
            return preset;
        }
    }

    return std::nullopt;
}
