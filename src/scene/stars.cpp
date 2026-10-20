#include "scene/stars.h"
#include "scene/celestial.h"

#include <algorithm>
#include <cmath>

namespace {
    // The visible range Planck's law is integrated over, and the step, in nanometres.
    constexpr float firstWavelength = 380.0f;
    constexpr float lastWavelength = 780.0f;
    constexpr float wavelengthStep = 5.0f;

    // Planck's second radiation constant, hc / k, in nanometre kelvins.
    constexpr double secondRadiationConstant = 1.4388e7;

    // One lobe of the colour matching functions' fit: a Gaussian with a different width either side of its peak.
    float lobe(float wavelength, float peak, float widthBelow, float widthAbove) {
        const float t = (wavelength - peak) / (wavelength < peak ? widthBelow : widthAbove);
        return std::exp(-0.5f * t * t);
    }

    // The CIE 1931 2-degree colour matching functions, Wyman, Sloan and Shirley's multi-lobe fit.
    Vec3 colourMatching(float wavelength) {
        const float x = 1.056f * lobe(wavelength, 599.8f, 37.9f, 31.0f) + 0.362f * lobe(wavelength, 442.0f, 16.0f, 26.7f) -
                        0.065f * lobe(wavelength, 501.1f, 20.4f, 26.2f);
        const float y = 0.821f * lobe(wavelength, 568.8f, 46.9f, 40.5f) + 0.286f * lobe(wavelength, 530.9f, 16.3f, 31.1f);
        const float z = 1.217f * lobe(wavelength, 437.0f, 11.8f, 36.0f) + 0.681f * lobe(wavelength, 459.0f, 26.0f, 13.8f);
        return Vec3{x, y, z};
    }

    // Planck's law up to a constant factor (it is normalised away): lambda^-5 / (exp(hc / (lambda k T)) - 1).
    double planck(float wavelength, float temperature) {
        const double lambda = static_cast<double>(wavelength);
        return 1.0 / (std::pow(lambda, 5.0) * std::expm1(secondRadiationConstant / (lambda * static_cast<double>(temperature))));
    }

    float luminance(const Vec3& colour) {
        return 0.2126f * colour.x + 0.7152f * colour.y + 0.0722f * colour.z;
    }
}

float starTemperature(float colourIndex) {
    const float scaled = 0.92f * colourIndex;
    return 4600.0f * (1.0f / (scaled + 1.7f) + 1.0f / (scaled + 0.62f));
}

/*
* XYZ to linear sRGB is IEC 61966-2-1's matrix. The luminance is set to 1 after clipping, so a clipped hue keeps the
* brightness the magnitude gives it.
*/
Vec3 blackbodyColour(float temperature) {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    for (float wavelength = firstWavelength; wavelength <= lastWavelength; wavelength += wavelengthStep) {
        const double power = planck(wavelength, temperature);
        const Vec3 matching = colourMatching(wavelength);
        x += power * matching.x;
        y += power * matching.y;
        z += power * matching.z;
    }

    const float X = static_cast<float>(x / y);
    const float Z = static_cast<float>(z / y);
    const Vec3 rgb{std::max(3.2406f * X - 1.5372f - 0.4986f * Z, 0.0f), std::max(-0.9689f * X + 1.8758f + 0.0415f * Z, 0.0f),
                   std::max(0.0557f * X - 0.2040f + 1.0570f * Z, 0.0f)};
    return rgb * (1.0f / luminance(rgb));
}

Star makeStar(const CatalogueStar& star) {
    return Star{equatorialDirection(star.rightAscension, star.declination), star.magnitude, blackbodyColour(starTemperature(star.colourIndex))};
}
