#include "render/gpu/depth_of_field.h"
#include "render/gpu/column_major.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
    constexpr float sensorHeight = static_cast<float>(DEPTH_OF_FIELD_SENSOR_HEIGHT);
    constexpr float largestRadius = static_cast<float>(DEPTH_OF_FIELD_LARGEST_RADIUS);
    constexpr float inFocus = static_cast<float>(DEPTH_OF_FIELD_IN_FOCUS);
    constexpr float edge = static_cast<float>(DEPTH_OF_FIELD_EDGE);
    constexpr float rings = static_cast<float>(DEPTH_OF_FIELD_RINGS);

    Vec3 mix(Vec3 a, Vec3 b, float t) {
        return a + (b - a) * t;
    }
}

// Half the sensor's height subtends half the field of view at the focal length.
float lensFocalLength(float verticalFovRadians) {
    return 0.5f * sensorHeight / std::tan(0.5f * verticalFovRadians);
}

// The image of a point at S lies v = S f / (S - f) behind the lens; the cone from the aperture A to it meets the sensor
// (at v1, the focus's image) as a disc of diameter A |v - v1| / v, which simplifies to A f / (S1 - f) |S2 - S1| / S2.
float circleOfConfusion(float distance, float focus, float focalLength, float fNumber) {
    const float aperture = focalLength / fNumber;
    return aperture * focalLength / (focus - focalLength) * (distance - focus) / distance;
}

// The radius (half the diameter) in blur pixels: the sensor's height spans the blur grid's, half the output's.
float depthOfFieldLensScale(float focalLength, float fNumber, float outputHeight) {
    const float aperture = focalLength / fNumber;
    return 0.5f * aperture * focalLength / sensorHeight * (0.5f * outputHeight);
}

float nearestFocusInverse(float focalLength) {
    return std::min(1.0f / static_cast<float>(DEPTH_OF_FIELD_NEAREST_FOCUS), static_cast<float>(DEPTH_OF_FIELD_FOCUS_LIMIT) / focalLength);
}

// The circle of confusion in inverse distances: A f (u - w) / (1 - f u), times the lens scale.
float blurRadius(float inverseDistance, float inverseFocus, float focalLength, float lensScale) {
    const float radius = lensScale * (inverseFocus - inverseDistance) / (1.0f - focalLength * inverseFocus);
    return std::clamp(radius, -largestRadius, largestRadius);
}

// dx/dt = (target - x) / tau, solved over the step: exact for any step, so frame rate does not change the motion.
float easeFocus(float current, float target, float seconds, float timeConstant) {
    if (timeConstant <= 0.0f) {
        return target;
    }

    return target + (current - target) * std::exp(-seconds / timeConstant);
}

float blurCoverage(float radius, float distance) {
    return std::clamp(std::abs(radius) - distance + edge, 0.0f, 1.0f);
}

Vec2 blurRingOffset(int ring, int step) {
    const float turn = (static_cast<float>(step) + (ring % 2 == 1 ? 0.5f : 0.0f)) / static_cast<float>(8 * ring);
    const float angle = turn * 2.0f * std::numbers::pi_v<float>;
    const float distance = (static_cast<float>(ring) - 0.5f) / rings;
    return Vec2{std::cos(angle), std::sin(angle)} * distance;
}

// Ring k's annulus, from (k - 1) / R to k / R, holds (2 k - 1) / R^2 of the unit disc's area, over its 8 k samples.
float blurRingShare(int ring) {
    if (ring <= 1) {
        return 1.0f / (9.0f * rings * rings);
    }

    const float k = static_cast<float>(ring);
    return (2.0f * k - 1.0f) / (8.0f * k * rings * rings);
}

float farSampleWeight(float sampleRadius, float pixelRadius, float distance) {
    if (sampleRadius < -inFocus) {
        return 0.0f;
    }

    const float reach = pixelRadius < -inFocus ? largestRadius : std::max(pixelRadius, 0.0f);
    return blurCoverage(std::min(std::max(sampleRadius, 0.0f), reach), distance);
}

float nearSampleWeight(float sampleRadius, float distance, float reach) {
    if (sampleRadius >= -inFocus) {
        return 0.0f;
    }

    return blurCoverage(sampleRadius, distance) * reach * reach / std::max(sampleRadius * sampleRadius, 1.0f);
}

// Sorted in place: nine values are few enough that the sort's cost does not matter on the CPU; the shader takes the
// same values through min and max.
float medianOfNine(std::array<float, 9> values) {
    std::sort(values.begin(), values.end());
    return values[4];
}

Vec3 depthOfFieldComposite(Vec3 sharp, float radius, Vec3 far, Vec3 nearColour, float nearAlpha) {
    const float blurred = std::clamp(std::abs(radius) / inFocus - 1.0f, 0.0f, 1.0f);
    return mix(mix(sharp, far, blurred), nearColour, nearAlpha);
}

DepthOfFieldTexel prepareBlurTexel(const std::array<Vec3, 4>& colours, const std::array<float, 4>& radii) {
    DepthOfFieldTexel texel;
    texel.colour = (colours[0] + colours[1] + colours[2] + colours[3]) * 0.25f;
    texel.radius = std::min(std::min(radii[0], radii[1]), std::min(radii[2], radii[3]));
    return texel;
}

DepthOfFieldLayers depthOfFieldAt(int x, int y, int width, int height, float nearReach, const DepthOfFieldPrepared& prepared) {
    const DepthOfFieldTexel centre = prepared(x, y);
    const float reach = std::max(std::abs(centre.radius), nearReach);
    DepthOfFieldLayers layers;
    layers.far = centre.colour;

    if (reach < inFocus) {
        return layers;
    }

    const float sampled = reach + edge;
    const float centreShare = blurRingShare(0);
    float farWeights = farSampleWeight(centre.radius, centre.radius, 0.0f) * centreShare;
    Vec3 farSum = centre.colour * farWeights;
    float nearWeights = nearSampleWeight(centre.radius, 0.0f, sampled) * centreShare;
    Vec3 nearSum = centre.colour * nearWeights;
    const Vec2 place{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};

    for (int ring = 1; ring <= DEPTH_OF_FIELD_RINGS; ++ring) {
        const float share = blurRingShare(ring);

        for (int step = 0; step < 8 * ring; ++step) {
            const Vec2 offset = blurRingOffset(ring, step) * sampled;
            const Vec2 at = place + offset;
            const int sx = std::clamp(static_cast<int>(std::floor(at.x)), 0, width - 1);
            const int sy = std::clamp(static_cast<int>(std::floor(at.y)), 0, height - 1);
            const DepthOfFieldTexel sample = prepared(sx, sy);
            const float distance = offset.length();
            const float farWeight = farSampleWeight(sample.radius, centre.radius, distance) * share;
            const float nearWeight = nearSampleWeight(sample.radius, distance, sampled) * share;
            farWeights += farWeight;
            farSum = farSum + sample.colour * farWeight;
            nearWeights += nearWeight;
            nearSum = nearSum + sample.colour * nearWeight;
        }
    }

    if (farWeights > 0.0f) {
        layers.far = farSum * (1.0f / farWeights);
    }

    if (nearWeights > 0.0f) {
        layers.nearColour = nearSum * (1.0f / nearWeights);
        layers.nearAlpha = std::min(nearWeights, 1.0f);
    }

    return layers;
}

RenderSize depthOfFieldGrid(RenderSize output) {
    return RenderSize{(output.width + 1) / 2, (output.height + 1) / 2};
}

RenderSize depthOfFieldTiles(RenderSize output) {
    const RenderSize grid = depthOfFieldGrid(output);
    const std::uint32_t tile = DEPTH_OF_FIELD_TILE;
    return RenderSize{(grid.width + tile - 1) / tile, (grid.height + tile - 1) / tile};
}

GpuDepthOfFieldPass packDepthOfFieldPass(const Mat4& inverseViewProjection, RenderSize render, RenderSize output, float verticalFovRadians,
                                         const CameraSettings& camera, float frameSeconds, bool snap) {
    GpuDepthOfFieldPass pass{};
    writeColumnMajor(inverseViewProjection, pass.inverseViewProjection);
    const auto sizeOf = [](RenderSize size, float (&into)[4]) {
        into[0] = static_cast<float>(size.width);
        into[1] = static_cast<float>(size.height);
        into[2] = 1.0f / static_cast<float>(size.width);
        into[3] = 1.0f / static_cast<float>(size.height);
    };
    sizeOf(render, pass.renderSize);
    sizeOf(output, pass.outputSize);
    const float focal = lensFocalLength(verticalFovRadians);
    pass.lens[0] = focal;
    pass.lens[1] = depthOfFieldLensScale(focal, camera.fNumber, static_cast<float>(output.height));
    pass.lens[2] = nearestFocusInverse(focal);
    pass.lens[3] = snap ? 1.0f : 0.0f;
    pass.focus[0] = 1.0f / camera.focusDistance;
    pass.focus[1] = camera.autofocus ? 1.0f : 0.0f;
    pass.focus[2] = frameSeconds;
    pass.focus[3] = camera.focusSeconds;
    pass.options[0] = camera.showFocus ? 1.0f : 0.0f;
    return pass;
}
