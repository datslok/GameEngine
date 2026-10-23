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
        int probeCascades = 0; // the light probes' cascades: how many, the densest's spacing, probes across and up the densest and each other
        float probeSpacing = 0.0f;
        int densestProbesAcross = 0;
        int densestProbesHigh = 0;
        int cascadeProbesAcross = 0;
        int cascadeProbesHigh = 0;
        bool reflections = false; // reflections, screen-space reflections' steps and the probe's size
        int reflectionSteps = 0;
        int probeSize = 0;

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
            .probeSharing = settings.globalIllumination.probeSharing,
            .probeCascades = settings.globalIllumination.probeCascades,
            .probeSpacing = settings.globalIllumination.probeSpacing,
            .densestProbesAcross = settings.globalIllumination.densestProbesAcross,
            .densestProbesHigh = settings.globalIllumination.densestProbesHigh,
            .cascadeProbesAcross = settings.globalIllumination.cascadeProbesAcross,
            .cascadeProbesHigh = settings.globalIllumination.cascadeProbesHigh,
            .reflections = settings.reflections.enabled,
            .reflectionSteps = settings.reflections.maxSteps,
            .probeSize = settings.reflections.probeSize
        };
    }

    // Ultra is read from the defaults, so retuning a default retunes Ultra with it, and High is Ultra with 256 probe rays
    // and a little sharing between probes to make up for them, and a reflection probe half as sharp.
    QualityValues presetValues(QualityPreset preset) {
        switch (preset) {
            case QualityPreset::High: {
                QualityValues high = valuesOf(RenderSettings{});
                high.probeRays = 256;
                high.fastProbeRays = 256;
                high.probeSharing = 0.1f;
                high.probeSize = 128;
                return high;
            }
            case QualityPreset::Low:
                return {.shadowedPoints = 8, .shadowedSpots = 4, .texelTarget = 0.1f, .spotTexelTarget = 0.05f,
                        .directionalDistance = 60.0f, .cascadeCount = 3,
                        .cascadeResolution = CascadeResolution::Pixels2048, .bloomLevels = 4, .probeRays = 32, .fastProbeRays = 32, .probeSharing = 0.5f,
                        .probeCascades = 4, .probeSpacing = 1.0f, .densestProbesAcross = 19, .densestProbesHigh = 11,
                        .cascadeProbesAcross = 15, .cascadeProbesHigh = 9, .reflections = false, .reflectionSteps = 32, .probeSize = 64};
            case QualityPreset::Medium:
                return {.shadowedPoints = 24, .shadowedSpots = 8, .texelTarget = 0.06f, .spotTexelTarget = 0.03f,
                        .directionalDistance = 120.0f, .cascadeCount = 3,
                        .cascadeResolution = CascadeResolution::Pixels2048, .bloomLevels = 5, .probeRays = 64, .fastProbeRays = 64, .probeSharing = 0.3f,
                        .probeCascades = 5, .probeSpacing = 0.5f, .densestProbesAcross = 25, .densestProbesHigh = 15,
                        .cascadeProbesAcross = 17, .cascadeProbesHigh = 11, .reflections = true, .reflectionSteps = 32, .probeSize = 64};
            case QualityPreset::Ultra:
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
    settings.globalIllumination.probeCascades = values.probeCascades;
    settings.globalIllumination.probeSpacing = values.probeSpacing;
    settings.globalIllumination.densestProbesAcross = values.densestProbesAcross;
    settings.globalIllumination.densestProbesHigh = values.densestProbesHigh;
    settings.globalIllumination.cascadeProbesAcross = values.cascadeProbesAcross;
    settings.globalIllumination.cascadeProbesHigh = values.cascadeProbesHigh;
    settings.reflections.enabled = values.reflections;
    settings.reflections.maxSteps = values.reflectionSteps;
    settings.reflections.probeSize = values.probeSize;
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
