#include "render/gpu/camera_finish.h"
#include "core/hash.h"
#include "render/gpu/eye_adaptation.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
    // The white balance's neutral: 6500 K light shows as it is.
    constexpr float neutralKelvin = 6500.0f;

    // The Planckian locus fit's range (Kim et al. 2002), and the tint's (Duv beyond this is no longer "white").
    constexpr float coolestKelvin = 1667.0f;
    constexpr float hottestKelvin = 25000.0f;
    constexpr float largestTint = 0.05f;

    // Linear sRGB (D65) to CIE XYZ, IEC 61966-2-1; XYZ to sRGB is its inverse, so the two compose to the identity.
    Mat4 srgbToXyz() {
        Mat4 matrix = Mat4::identity();
        const float rows[3][3] = {{0.4124564f, 0.3575761f, 0.1804375f}, {0.2126729f, 0.7151522f, 0.0721750f}, {0.0193339f, 0.1191920f, 0.9503041f}};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                matrix.values[row][column] = rows[row][column];
            }
        }
        return matrix;
    }

    // The Bradford matrix: CIE XYZ to the sharpened cone responses chromatic adaptation scales.
    Mat4 bradford() {
        Mat4 matrix = Mat4::identity();
        const float rows[3][3] = {{0.8951f, 0.2664f, -0.1614f}, {-0.7502f, 1.7135f, 0.0367f}, {0.0389f, -0.0685f, 1.0296f}};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                matrix.values[row][column] = rows[row][column];
            }
        }
        return matrix;
    }

    Vec3 times(const Mat4& matrix, const Vec3& vector) {
        const Vec4 result = matrix * Vec4{vector.x, vector.y, vector.z, 0.0f};
        return Vec3{result.x, result.y, result.z};
    }

    Vec3 xyzOf(Vec2 xy, float luminance) {
        return Vec3{xy.x / xy.y * luminance, luminance, (1.0f - xy.x - xy.y) / xy.y * luminance};
    }

    // CIE 1960 uv, in which Duv is measured, and back.
    Vec2 uvOf(Vec2 xy) {
        const float d = -2.0f * xy.x + 12.0f * xy.y + 3.0f;
        return Vec2{4.0f * xy.x / d, 6.0f * xy.y / d};
    }

    Vec2 xyOf(Vec2 uv) {
        const float d = 2.0f * uv.x - 8.0f * uv.y + 4.0f;
        return Vec2{3.0f * uv.x / d, 2.0f * uv.y / d};
    }

    void check(bool valid, const char* message) {
        if (!valid) {
            throw std::invalid_argument(message);
        }
    }
}

void visit(CameraSettings& settings, SettingsVisitor& visitor) {
    visitor.field("White balance", settings.whiteBalance, {.min = coolestKelvin, .max = hottestKelvin, .sliderMin = 2000.0f, .sliderMax = 12000.0f, .unit = "K"});
    visitor.field("Tint", settings.tint, {.min = -largestTint, .max = largestTint});
    visitor.field("Contrast", settings.contrast, {.min = 0.01f, .max = 10.0f, .sliderMin = 0.5f, .sliderMax = 2.0f});
    visitor.field("Saturation", settings.saturation, {.min = 0.0f, .max = 10.0f, .sliderMin = 0.0f, .sliderMax = 2.0f});
    visitor.field("Lift", settings.lift, {.min = -1.0f, .max = 1.0f, .sliderMin = -0.1f, .sliderMax = 0.1f});
    visitor.field("Gamma", settings.gamma, {.min = 0.01f, .max = 10.0f, .sliderMin = 0.5f, .sliderMax = 2.0f});
    visitor.field("Gain", settings.gain, {.min = 0.0f, .max = 10.0f, .sliderMin = 0.5f, .sliderMax = 2.0f});
    visitor.field("Vignetting", settings.vignetting, {.min = 0.0f, .max = 1.0f});
    visitor.field("Chromatic aberration", settings.chromaticAberration, {.min = 0.0f, .max = 50.0f, .sliderMin = 0.0f, .sliderMax = 8.0f, .unit = "px"});
    visitor.field("Film grain", settings.filmGrain, {.min = 0.0f, .max = 1.0f, .sliderMin = 0.0f, .sliderMax = 0.3f});
    visitor.field("Motion blur", settings.motionBlur);
    visitor.field("Shutter angle", settings.shutterAngle, {.min = 1.0f, .max = 360.0f, .unit = "degrees"});
    visitor.field("Camera motion blur", settings.cameraMotionBlur);
}

void validateCamera(const CameraSettings& settings) {
    check(settings.whiteBalance >= coolestKelvin && settings.whiteBalance <= hottestKelvin, "The white balance must be 1667 to 25000 K");
    check(std::abs(settings.tint) <= largestTint, "The tint must be within 0.05 of the blackbody locus");
    check(settings.contrast > 0.0f && std::isfinite(settings.contrast), "The contrast must be above 0");
    check(settings.saturation >= 0.0f && std::isfinite(settings.saturation), "The saturation must be 0 or more");
    check(std::abs(settings.lift) <= 1.0f, "The lift must be within -1..1");
    check(settings.gamma > 0.0f && std::isfinite(settings.gamma), "The gamma must be above 0");
    check(settings.gain >= 0.0f && std::isfinite(settings.gain), "The gain must be 0 or more");
    check(settings.vignetting >= 0.0f && settings.vignetting <= 1.0f, "The vignetting must be 0..1");
    check(settings.chromaticAberration >= 0.0f && settings.chromaticAberration <= 50.0f, "The chromatic aberration must be 0 to 50 pixels");
    check(settings.filmGrain >= 0.0f && settings.filmGrain <= 1.0f, "The film grain must be 0..1");
    check(settings.shutterAngle >= 1.0f && settings.shutterAngle <= 360.0f, "The shutter angle must be 1 to 360 degrees");
}

bool gradesColour(const CameraSettings& settings) {
    const CameraSettings neutral;
    return settings.whiteBalance != neutral.whiteBalance || settings.tint != neutral.tint || settings.contrast != neutral.contrast ||
           settings.saturation != neutral.saturation || settings.lift != neutral.lift || settings.gamma != neutral.gamma || settings.gain != neutral.gain;
}

// x as a cubic in 1 / T, then y as a cubic in x, each in its range of temperatures.
Vec2 planckianChromaticity(float kelvin) {
    const double t = kelvin;
    const double inverse = 1.0 / t;
    const double x = t <= 4000.0 ? -0.2661239e9 * inverse * inverse * inverse - 0.2343589e6 * inverse * inverse + 0.8776956e3 * inverse + 0.179910
                                 : -3.0258469e9 * inverse * inverse * inverse + 2.1070379e6 * inverse * inverse + 0.2226347e3 * inverse + 0.240390;
    double y = 0.0;

    if (t <= 2222.0) {
        y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
    } else if (t <= 4000.0) {
        y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
    }

    return Vec2{static_cast<float>(x), static_cast<float>(y)};
}

// The locus's direction from the temperatures a kelvin either side; across it is that turned a quarter, pointing to
// higher v (greener).
Vec2 whiteBalanceChromaticity(float kelvin, float tint) {
    if (tint == 0.0f) {
        return planckianChromaticity(kelvin);
    }

    const Vec2 uv = uvOf(planckianChromaticity(kelvin));
    const Vec2 along = (uvOf(planckianChromaticity(kelvin + 1.0f)) - uvOf(planckianChromaticity(kelvin - 1.0f))).normalized();
    Vec2 across{-along.y, along.x};

    if (across.y < 0.0f) {
        across = across * -1.0f;
    }

    return xyOf(uv + across * tint);
}

Vec3 chromaticityToLinearSrgb(Vec2 xy, float luminance) {
    return times(*inverse(srgbToXyz()), xyzOf(xy, luminance));
}

/*
* sRGB to XYZ, to cone responses, each scaled by the ratio of 6500 K light's response to the chosen light's, back to XYZ
* and to sRGB. At 6500 K and no tint the scales are 1 and the product is the identity but for rounding, so the identity
* itself is returned there.
*/
Mat4 whiteBalanceMatrix(float kelvin, float tint) {
    const Mat4 toXyz = srgbToXyz();
    const Mat4 cones = bradford();
    const Vec3 source = times(cones, xyzOf(whiteBalanceChromaticity(kelvin, tint), 1.0f));
    const Vec3 target = times(cones, xyzOf(planckianChromaticity(neutralKelvin), 1.0f));
    const Mat4 scale = Mat4::scaling(target.x / source.x, target.y / source.y, target.z / source.z);
    Mat4 result = *inverse(toXyz) * *inverse(cones) * scale * cones * toXyz;

    if (kelvin == neutralKelvin && tint == 0.0f) {
        result = Mat4::identity();
    }

    return result;
}

Vec3 gradeColour(const Vec3& exposed, const CameraSettings& settings) {
    const float midGrey = static_cast<float>(CAMERA_MID_GREY);
    const auto contrasted = [&](float channel) { return midGrey * std::pow(std::max(channel, 0.0f) / midGrey, settings.contrast); };
    Vec3 colour{contrasted(exposed.x), contrasted(exposed.y), contrasted(exposed.z)};
    const float luminance = pixelLuminance(colour);
    const auto saturated = [&](float channel) { return std::max(luminance + (channel - luminance) * settings.saturation, 0.0f); };
    colour = Vec3{saturated(colour.x), saturated(colour.y), saturated(colour.z)};
    const auto decided = [&](float channel) { return std::pow(std::max(channel * settings.gain + settings.lift, 0.0f), 1.0f / settings.gamma); };
    return Vec3{decided(colour.x), decided(colour.y), decided(colour.z)};
}

// 1 / (1 + tan^2)^2 is cos^4 without the angle itself.
float naturalVignetting(Vec2 ndc, Vec2 tanHalfFov, float strength) {
    const float acrossTan = ndc.x * tanHalfFov.x;
    const float upTan = ndc.y * tanHalfFov.y;
    const float tanSquared = acrossTan * acrossTan + upTan * upTan;
    const float cosFourth = 1.0f / ((1.0f + tanSquared) * (1.0f + tanSquared));
    return 1.0f + (cosFourth - 1.0f) * strength;
}

Vec2 aberrationPlace(Vec2 pixel, Vec2 size, float shiftPixels) {
    const Vec2 centre = size * 0.5f;
    return centre + (pixel - centre) * (1.0f + shiftPixels / centre.length());
}

float grainNoise(std::uint32_t x, std::uint32_t y, std::uint32_t frame) {
    return static_cast<float>(pcg3d({x, y, frame})[0] >> 8u) * (1.0f / 16777216.0f);
}

Vec3 filmGrain(const Vec3& mapped, float noise, float strength, float peak) {
    const float level = std::clamp(pixelLuminance(mapped) / peak, 0.0f, 1.0f);
    const float grain = (noise - 0.5f) * strength * 4.0f * level * (1.0f - level) * peak;
    return Vec3{std::clamp(mapped.x + grain, 0.0f, peak), std::clamp(mapped.y + grain, 0.0f, peak), std::clamp(mapped.z + grain, 0.0f, peak)};
}
