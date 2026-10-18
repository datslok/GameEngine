#include "render/gpu/tone_mapping.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

void validateToneMapping(const ToneMappingSettings& settings, float peakBrightness) {
    if (!(settings.exposure >= 0.0f)) {
        throw std::invalid_argument("Tone mapping exposure must not be negative");
    }

    if (!(peakBrightness > 0.0f)) {
        throw std::invalid_argument("Tone mapping peak brightness must be positive");
    }
}

namespace {
    float reinhard(float value) {
        return value / (1.0f + value);
    }

    float aces(float value) {
        const float numerator = value * (static_cast<float>(ACES_A) * value + static_cast<float>(ACES_B));
        const float denominator = value * (static_cast<float>(ACES_C) * value + static_cast<float>(ACES_D)) + static_cast<float>(ACES_E);
        return numerator / denominator;
    }

    // A 3x3 matrix given by rows, times a column vector.
    Vec3 multiply(const float (&rows)[3][3], const Vec3& value) {
        return Vec3{
            rows[0][0] * value.x + rows[0][1] * value.y + rows[0][2] * value.z,
            rows[1][0] * value.x + rows[1][1] * value.y + rows[1][2] * value.z,
            rows[2][0] * value.x + rows[2][1] * value.y + rows[2][2] * value.z
        };
    }

    // The S-curve over the stops range, t in 0..1, by Horner's rule.
    float agxContrast(float t) {
        float result = static_cast<float>(AGX_C6);
        result = result * t + static_cast<float>(AGX_C5);
        result = result * t + static_cast<float>(AGX_C4);
        result = result * t + static_cast<float>(AGX_C3);
        result = result * t + static_cast<float>(AGX_C2);
        result = result * t + static_cast<float>(AGX_C1);
        return result * t + static_cast<float>(AGX_C0);
    }

    // One channel to its place on the curve: the brightness in stops, clamped to the covered range, rescaled to 0..1.
    // Black is log2 of zero, minus infinity, so a tiny floor keeps it finite; it lands at the bottom of the range.
    float agxChannel(float value) {
        const float minStops = static_cast<float>(AGX_MIN_STOPS);
        const float maxStops = static_cast<float>(AGX_MAX_STOPS);
        const float stops = std::clamp(std::log2(std::max(value, 1e-10f)), minStops, maxStops);

        return agxContrast((stops - minStops) / (maxStops - minStops));
    }

    // Back to linear light: undo the inset, then the display gamma. Out of range values from the polynomial's ends
    // are clamped first, since a negative number has no real power.
    float agxToLinear(float encoded) {
        return std::pow(std::clamp(encoded, 0.0f, 1.0f), static_cast<float>(AGX_DISPLAY_GAMMA));
    }

    /*
    * AgX works in a log space (stops), like a photographer's light meter, so the whole 16.5-stop range gets the same
    * treatment. The inset first mixes a little of each channel into the others, so a very bright pure red light still
    * reaches the curve's top in green and blue too and rolls off towards white, as an overexposed red light does on film.
    */
    Vec3 agx(const Vec3& value) {
        constexpr float inset[3][3] = {
            {static_cast<float>(AGX_INSET_00), static_cast<float>(AGX_INSET_01), static_cast<float>(AGX_INSET_02)},
            {static_cast<float>(AGX_INSET_10), static_cast<float>(AGX_INSET_11), static_cast<float>(AGX_INSET_12)},
            {static_cast<float>(AGX_INSET_20), static_cast<float>(AGX_INSET_21), static_cast<float>(AGX_INSET_22)}
        };
        constexpr float outset[3][3] = {
            {static_cast<float>(AGX_OUTSET_00), static_cast<float>(AGX_OUTSET_01), static_cast<float>(AGX_OUTSET_02)},
            {static_cast<float>(AGX_OUTSET_10), static_cast<float>(AGX_OUTSET_11), static_cast<float>(AGX_OUTSET_12)},
            {static_cast<float>(AGX_OUTSET_20), static_cast<float>(AGX_OUTSET_21), static_cast<float>(AGX_OUTSET_22)}
        };

        const Vec3 inside = multiply(inset, value);
        const Vec3 curved{agxChannel(inside.x), agxChannel(inside.y), agxChannel(inside.z)};
        const Vec3 encoded = multiply(outset, curved);

        return Vec3{agxToLinear(encoded.x), agxToLinear(encoded.y), agxToLinear(encoded.z)};
    }

    // The curve for a screen whose peak is 1.
    Vec3 applyCurve(ToneMapCurve curve, const Vec3& value) {
        switch (curve) {
        case ToneMapCurve::Clamp:
            return Vec3{std::min(value.x, 1.0f), std::min(value.y, 1.0f), std::min(value.z, 1.0f)};
        case ToneMapCurve::Reinhard:
            return Vec3{reinhard(value.x), reinhard(value.y), reinhard(value.z)};
        case ToneMapCurve::Aces:
            return Vec3{aces(value.x), aces(value.y), aces(value.z)};
        case ToneMapCurve::AgX:
            return agx(value);
        }

        throw std::invalid_argument("Unknown tone mapping curve");
    }
}

/*
* Exposure first (it is part of the scene's brightness), then the curve, stretched to the screen's peak: measuring the
* light in units of the peak, curving, and scaling back keeps the look the same on any screen, just brighter at the top.
* Nothing is allowed below black or past the peak.
*/
Vec3 toneMap(const Vec3& linear, const ToneMappingSettings& settings, float peakBrightness) {
    validateToneMapping(settings, peakBrightness);

    const float scale = settings.exposure / peakBrightness;
    const Vec3 curved = applyCurve(settings.curve, Vec3{
        std::max(linear.x, 0.0f) * scale,
        std::max(linear.y, 0.0f) * scale,
        std::max(linear.z, 0.0f) * scale
    });

    return Vec3{
        std::clamp(curved.x, 0.0f, 1.0f) * peakBrightness,
        std::clamp(curved.y, 0.0f, 1.0f) * peakBrightness,
        std::clamp(curved.z, 0.0f, 1.0f) * peakBrightness
    };
}

namespace {
    // In the order of ToneMapCurve's values.
    constexpr std::array<const char*, 4> curveNames{"Clamp", "Reinhard", "ACES", "AgX"};

    static_assert(static_cast<int>(ToneMapCurve::Clamp) == 0 && static_cast<int>(ToneMapCurve::AgX) == 3,
                  "curveNames lists the curves by their values");
}

void visit(ToneMappingSettings& settings, SettingsVisitor& visitor) {
    visitor.choice("Curve", settings.curve, curveNames);
    visitor.field("Exposure", settings.exposure, {.min = 0.0f, .sliderMax = 4.0f});
}
