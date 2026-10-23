#include "render/gpu/upscaling.h"
#include "render/gpu/column_major.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
    // AMD's and NVIDIA's recommended mip bias for FSR and DLSS is a level sharper than the scale's own.
    constexpr float vendorExtraLodBias = -1.0f;

    // FSR's jitter cycle at full size, growing with the square of the upscaling.
    constexpr double fsrBasePhaseCount = 8.0;

    // Each mode's upscaling, the output's size over the render's, in UpscalingMode's order (AMD's and NVIDIA's figures).
    constexpr std::array<float, 5> modeUpscaling{1.0f, 1.5f, 1.7f, 2.0f, 3.0f};

    // Half the render scale slider's step of a hundredth, so a scale dragged to the nearest hundredth reads as its mode.
    constexpr float modeTolerance = 0.005f;
}

void visit(UpscalingSettings& settings, SettingsVisitor& visitor) {
    visitor.choice("Anti-aliasing", settings.upscaler, upscalerNames);
    visitor.field("Render scale", settings.renderScale, {.min = minRenderScale, .max = 1.0f});
    visitor.field("FSR sharpness", settings.sharpness, {.min = 0.0f, .max = 1.0f});
}

// Written so not a number fails them too.
void validateUpscaling(const UpscalingSettings& settings) {
    if (!(settings.renderScale >= minRenderScale && settings.renderScale <= 1.0f)) {
        throw std::invalid_argument("Upscaling needs a render scale from 0.33 to 1");
    }

    if (!(settings.sharpness >= 0.0f && settings.sharpness <= 1.0f)) {
        throw std::invalid_argument("Upscaling needs a sharpness from 0 to 1");
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

bool temporalAntiAliasing(const UpscalingSettings& settings) {
    return settings.upscaler != Upscaler::Off;
}

float appliedRenderScale(const UpscalingSettings& settings) {
    return temporalAntiAliasing(settings) ? settings.renderScale : 1.0f;
}

float upscalingModeScale(UpscalingMode mode) {
    return 1.0f / modeUpscaling[static_cast<std::size_t>(mode)];
}

std::optional<UpscalingMode> matchingUpscalingMode(float renderScale) {
    for (std::size_t index = 0; index < modeUpscaling.size(); ++index) {
        const UpscalingMode mode = static_cast<UpscalingMode>(index);

        if (std::abs(renderScale - upscalingModeScale(mode)) <= modeTolerance) {
            return mode;
        }
    }

    return std::nullopt;
}

// The scales are compared as they are, not as their upscalings: nearer in what fraction of the output is drawn.
UpscalingMode nearestUpscalingMode(float renderScale) {
    UpscalingMode nearest = UpscalingMode::Native;

    for (std::size_t index = 0; index < modeUpscaling.size(); ++index) {
        const UpscalingMode mode = static_cast<UpscalingMode>(index);

        if (std::abs(renderScale - upscalingModeScale(mode)) < std::abs(renderScale - upscalingModeScale(nearest))) {
            nearest = mode;
        }
    }

    return nearest;
}

Upscaler activeUpscaler(const UpscalingSettings& settings, UpscalerSupport support) {
    if (settings.upscaler == Upscaler::Off) {
        return Upscaler::Off;
    }

    if (settings.upscaler == Upscaler::Fsr && support.fsr) {
        return Upscaler::Fsr;
    }

    if (settings.upscaler == Upscaler::Dlss && support.dlss) {
        return Upscaler::Dlss;
    }

    return Upscaler::Taa;
}

RenderSize clampRenderSize(RenderSize size, RenderSize smallest, RenderSize largest) {
    return RenderSize{std::clamp(size.width, smallest.width, std::max(smallest.width, largest.width)),
                      std::clamp(size.height, smallest.height, std::max(smallest.height, largest.height))};
}

// From the sizes as rounded, so the bias matches the pixels actually drawn.
float textureLodBias(RenderSize render, std::uint32_t outputWidth, Upscaler upscaler) {
    const float extra = upscaler == Upscaler::Fsr || upscaler == Upscaler::Dlss ? vendorExtraLodBias : 0.0f;

    if (render.width == outputWidth || outputWidth == 0) {
        return extra;
    }

    return std::log2(static_cast<float>(render.width) / static_cast<float>(outputWidth)) + extra;
}

// In doubles, so exact ratios (2, 3) give exact counts rather than one more from rounding.
std::uint32_t jitterPhaseCount(RenderSize render, std::uint32_t outputWidth) {
    const double ratio = static_cast<double>(std::max(outputWidth, std::uint32_t{1})) / static_cast<double>(std::max(render.width, std::uint32_t{1}));
    return static_cast<std::uint32_t>(std::ceil(fsrBasePhaseCount * ratio * ratio));
}

/*
* FSR reads a motion vector m as m times the scale, in render pixels from where the pixel is now to where it was. Ours
* is NDC now minus NDC then; a pixel is 2 / width of NDC across and counts down the screen while NDC y counts up, so
* the step back in pixels is -m.x width / 2 across and +m.y height / 2 down. With reversed-Z, FSR names the planes by
* the depths they hold: the far plane, at depth 0, is its "near" (it sorts them itself, but its checks warn otherwise).
*/
FsrFrameInputs makeFsrFrameInputs(RenderSize render, std::uint32_t outputWidth, std::uint32_t outputHeight, Vec2 jitter, float frameSeconds,
                                  float cameraNear, float cameraFar, float verticalFov, bool reset, const UpscalingSettings& settings) {
    FsrFrameInputs inputs;
    inputs.renderSize = render;
    inputs.outputWidth = outputWidth;
    inputs.outputHeight = outputHeight;
    inputs.jitter = jitter;
    inputs.motionVectorScale = Vec2{-0.5f * static_cast<float>(render.width), 0.5f * static_cast<float>(render.height)};
    inputs.frameMilliseconds = frameSeconds * 1000.0f;
    inputs.sharpen = settings.sharpness > 0.0f;
    inputs.sharpness = settings.sharpness;
    inputs.cameraNear = cameraFar;
    inputs.cameraFar = cameraNear;
    inputs.verticalFov = verticalFov;
    inputs.reset = reset;
    return inputs;
}

std::array<float, 16> streamlineMatrix(const Mat4& matrix) {
    std::array<float, 16> values{};
    writeColumnMajor(matrix, values.data());
    return values;
}

/*
* Streamline normalises a motion to the screen (its scale makes it a share of the width and height), from where the
* pixel is now to where it was: UV = (NDC x + 1) / 2 across and (1 - NDC y) / 2 down, so our NDC now minus then becomes
* -m.x / 2 and +m.y / 2. A matrix that cannot be inverted (never a real lens or camera step) gives the identity.
*/
DlssFrameInputs makeDlssFrameInputs(RenderSize render, std::uint32_t outputWidth, std::uint32_t outputHeight, Vec2 jitter, const Mat4& viewToClip,
                                    const Mat4& reprojection, const DlssCamera& camera, bool reset) {
    DlssFrameInputs inputs;
    inputs.renderSize = render;
    inputs.outputWidth = outputWidth;
    inputs.outputHeight = outputHeight;
    inputs.jitter = jitter;
    inputs.motionVectorScale = Vec2{-0.5f, 0.5f};
    inputs.viewToClip = streamlineMatrix(viewToClip);
    inputs.clipToView = streamlineMatrix(inverse(viewToClip).value_or(Mat4::identity()));
    inputs.clipToPrevClip = streamlineMatrix(reprojection);
    inputs.prevClipToClip = streamlineMatrix(inverse(reprojection).value_or(Mat4::identity()));
    inputs.camera = camera;
    inputs.reset = reset;
    return inputs;
}
