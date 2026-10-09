#include "core/srgb.h"

#include <algorithm>
#include <cmath>

/*
* The official sRGB curve: a straight line near black (so the slope stays finite), then a 2.4 power.
* Together they track a plain 2.2 gamma closely.
*/
float srgbToLinear(float encoded) {
    if (encoded <= 0.04045f) {
        return encoded / 12.92f;
    }

    return std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

/*
* The exact inverse of srgbToLinear, with the switch point moved to where the line meets the curve in linear terms.
*/
float linearToSrgb(float linear) {
    if (linear <= 0.0031308f) {
        return linear * 12.92f;
    }

    return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

float srgbByteToLinear(std::uint8_t encoded) {
    return srgbToLinear(static_cast<float>(encoded) / 255.0f);
}

/*
* Clamped first, because light can exceed 1 but a byte cannot. Rounded, so a decoded byte comes back unchanged.
*/
std::uint8_t linearToSrgbByte(float linear) {
    const float encoded = linearToSrgb(std::clamp(linear, 0.0f, 1.0f));

    return static_cast<std::uint8_t>(std::lround(encoded * 255.0f));
}
