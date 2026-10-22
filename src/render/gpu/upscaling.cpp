#include "render/gpu/upscaling.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

void visit(UpscalingSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Render scale", settings.renderScale, {.min = minRenderScale, .max = 1.0f});
}

// Written so not a number fails it too.
void validateUpscaling(const UpscalingSettings& settings) {
    if (!(settings.renderScale >= minRenderScale && settings.renderScale <= 1.0f)) {
        throw std::invalid_argument("Upscaling needs a render scale from 0.33 to 1");
    }
}

namespace {
    std::uint32_t scaledSide(std::uint32_t output, float scale) {
        const float scaled = std::round(static_cast<float>(output) * scale);
        return std::clamp(static_cast<std::uint32_t>(std::max(scaled, 1.0f)), std::uint32_t{1}, std::max(output, std::uint32_t{1}));
    }
}

RenderSize renderSize(std::uint32_t outputWidth, std::uint32_t outputHeight, float scale) {
    return RenderSize{scaledSide(outputWidth, scale), scaledSide(outputHeight, scale)};
}

float appliedRenderScale(const UpscalingSettings& settings, bool temporalAntiAliasing) {
    return temporalAntiAliasing ? settings.renderScale : 1.0f;
}

// From the sizes as rounded, so the bias matches the pixels actually drawn.
float textureLodBias(RenderSize render, std::uint32_t outputWidth) {
    if (render.width == outputWidth || outputWidth == 0) {
        return 0.0f;
    }

    return std::log2(static_cast<float>(render.width) / static_cast<float>(outputWidth));
}
