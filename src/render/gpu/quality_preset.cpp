#include "render/gpu/quality_preset.h"

namespace {
    // Everything a preset sets, in one place, so applying and matching cannot disagree.
    struct QualityValues {
        int shadowedPoints = 0;
        int shadowedSpots = 0;
        float texelTarget = 0.0f;
        float spotTexelTarget = 0.0f;
        float directionalDistance = 0.0f;
        int cascadeCount = 0;
        CascadeResolution cascadeResolution = CascadeResolution::Pixels2048;
        std::uint32_t bloomLevels = 0;
        int probeRays = 0;     // the light probes' rays, near fast lights or not
        int fastProbeRays = 0;
        float probeSharing = 0.0f;

        bool operator==(const QualityValues&) const = default;
    };

    QualityValues valuesOf(const RenderSettings& settings) {
        return {
            .shadowedPoints = settings.lights.shadowedPoints,
            .shadowedSpots = settings.lights.shadowedSpots,
            .texelTarget = settings.shadows.texelTarget,
            .spotTexelTarget = settings.shadows.spotTexelTarget,
            .directionalDistance = settings.shadows.directionalDistance,
            .cascadeCount = settings.shadows.cascadeCount,
            .cascadeResolution = settings.shadows.cascadeResolution,
            .bloomLevels = settings.bloom.maxLevels,
            .probeRays = settings.globalIllumination.raysPerProbe,
            .fastProbeRays = settings.globalIllumination.fastRaysPerProbe,
            .probeSharing = settings.globalIllumination.probeSharing
        };
    }

    // High is read from the defaults, so retuning a default retunes High with it, and Ultra is High with 256 probe rays
    // and no sharing between probes (enough rays of their own).
    QualityValues presetValues(QualityPreset preset) {
        switch (preset) {
            case QualityPreset::Ultra: {
                QualityValues ultra = valuesOf(RenderSettings{});
                ultra.probeRays = 256;
                ultra.fastProbeRays = 256;
                ultra.probeSharing = 0.0f;
                return ultra;
            }
            case QualityPreset::Low:
                return {.shadowedPoints = 8, .shadowedSpots = 4, .texelTarget = 0.1f, .spotTexelTarget = 0.05f,
                        .directionalDistance = 60.0f, .cascadeCount = 3,
                        .cascadeResolution = CascadeResolution::Pixels2048, .bloomLevels = 4, .probeRays = 32, .fastProbeRays = 32, .probeSharing = 0.5f};
            case QualityPreset::Medium:
                return {.shadowedPoints = 24, .shadowedSpots = 8, .texelTarget = 0.06f, .spotTexelTarget = 0.03f,
                        .directionalDistance = 120.0f, .cascadeCount = 3,
                        .cascadeResolution = CascadeResolution::Pixels2048, .bloomLevels = 5, .probeRays = 64, .fastProbeRays = 64, .probeSharing = 0.3f};
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
    settings.shadows.cascadeCount = values.cascadeCount;
    settings.shadows.cascadeResolution = values.cascadeResolution;
    settings.bloom.maxLevels = values.bloomLevels;
    settings.globalIllumination.raysPerProbe = values.probeRays;
    settings.globalIllumination.fastRaysPerProbe = values.fastProbeRays;
    settings.globalIllumination.probeSharing = values.probeSharing;
}

std::optional<QualityPreset> matchingQualityPreset(const RenderSettings& settings) {
    const QualityValues current = valuesOf(settings);

    for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High, QualityPreset::Ultra}) {
        if (presetValues(preset) == current) {
            return preset;
        }
    }

    return std::nullopt;
}
