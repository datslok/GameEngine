#include "render/gpu/typical_gpu.h"
#include "render/gpu/quality_preset.h"

Upscaler typicalUpscaler(UpscalerSupport support) {
    if (support.dlss) {
        return Upscaler::Dlss;
    }

    return support.fsr ? Upscaler::Fsr : Upscaler::Taa;
}

void applyTypicalGpu(RenderSettings& settings, UpscalerSupport support) {
    applyQualityPreset(settings, QualityPreset::High);
    settings.rayTracing = false;
    settings.upscaling.upscaler = typicalUpscaler(support);
    settings.upscaling.renderScale = upscalingModeScale(UpscalingMode::Quality);
}

bool TypicalGpuView::isOn() const {
    return saved.has_value();
}

void TypicalGpuView::turnOn(RenderSettings& settings, UpscalerSupport support) {
    if (!saved) {
        saved = settings;
    }

    applyTypicalGpu(settings, support);
}

void TypicalGpuView::turnOff(RenderSettings& settings) {
    if (!saved) {
        return;
    }

    settings = *saved;
    saved.reset();
}
