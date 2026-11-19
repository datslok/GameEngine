#include "render/gpu/window_mask.h"

#include "core/hash.h"
#include "math/smooth_step.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
    constexpr float seedSteps = static_cast<float>(WINDOW_SEED_STEPS);
    constexpr float leastBrightness = static_cast<float>(WINDOW_LEAST_BRIGHTNESS);
    constexpr float brightnessRange = static_cast<float>(WINDOW_BRIGHTNESS_RANGE);

    // A hashed word's top 24 bits as a fraction 0..1, exact in a float.
    float unitOf(std::uint32_t word) {
        return static_cast<float>(word >> 8u) * (1.0f / 16777216.0f);
    }

    Vec3 mixColour(const Vec3& a, const Vec3& b, float share) {
        return a + (b - a) * share;
    }
}

/*
* Whole cells give length each; within the last cell, the part past the slit's start (its middle less half its length),
* up to its length.
*/
float pulseIntegral(float x, float pitch, float length) {
    const float cell = std::floor(x / pitch);
    const float inCell = x - cell * pitch;
    const float start = 0.5f * (pitch - length);
    return cell * length + std::clamp(inCell - start, 0.0f, length);
}

// The integral's difference over the footprint, over its width: the exact share a box filter sees.
float pulseCoverage(float centre, float footprint, float pitch, float length) {
    if (footprint <= 1e-4f * pitch) {
        const float inCell = centre - std::floor(centre / pitch) * pitch;
        const float start = 0.5f * (pitch - length);
        return inCell >= start && inCell < start + length ? 1.0f : 0.0f;
    }

    return (pulseIntegral(centre + 0.5f * footprint, pitch, length) - pulseIntegral(centre - 0.5f * footprint, pitch, length)) / footprint;
}

std::uint32_t windowFaceSeed(Vec2 facetNumbers) {
    const auto step = [](float number) {
        return static_cast<std::uint32_t>(std::floor(number * seedSteps + 0.5f));
    };
    return step(facetNumbers.x) | (step(facetNumbers.y) << 11u);
}

// pcg3d of the cell's column and row as integers (their bits kept when negative) and the face's seed.
Vec3 windowHash(float across, float height, float pitch, std::uint32_t faceSeed) {
    const auto column = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(across / pitch)));
    const auto row = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(height / windowRowHeight)));
    const std::array<std::uint32_t, 3> hashed = pcg3d({column, row, faceSeed});
    return Vec3{unitOf(hashed[0]), unitOf(hashed[1]), unitOf(hashed[2])};
}

float windowFarShare(Vec2 footprint, float pitch) {
    const float cells = std::max(footprint.x / pitch, footprint.y / windowRowHeight);
    return smoothStep(windowFarStart, windowFarFull, cells);
}

/*
* Near, a window is lit when its first number is under the share lit, cool when its second is under the cool share, and
* as bright as its third says (mean 1); far, each is its expected value, so the band keeps its average as it fades.
*/
Vec3 windowEmission(Vec2 place, Vec2 footprint, const Vec4& band, float pitch, std::uint32_t faceSeed, const Vec3& warm, const Vec3& cool) {
    if (!(band.x > 0.0f) || !(band.w > 0.0f) || !(pitch > 0.0f)) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }

    const float across = pulseCoverage(place.x, footprint.x, pitch, band.w * pitch);
    const Vec3 numbers = windowHash(place.x, place.y, pitch, faceSeed);
    const float far = windowFarShare(footprint, pitch);
    const float lit = (numbers.x < band.y ? 1.0f : 0.0f) * (1.0f - far) + band.y * far;
    const Vec3 own = numbers.y < band.z ? cool : warm;
    const Vec3 colour = mixColour(own, mixColour(warm, cool, band.z), far);
    const float brightness = (leastBrightness + brightnessRange * numbers.z) * (1.0f - far) + far;
    return colour * (band.x * across * lit * brightness);
}
