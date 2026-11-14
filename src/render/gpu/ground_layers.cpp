#include "render/gpu/ground_layers.h"

#include "math/smooth_step.h"

#include <algorithm>
#include <cmath>

/*
* With a blend depth of 0 only the highest layer's share stays, and that is zero too (it comes within 0 of itself), so
* the painted weights alone are used, normalised.
*/
GroundWeights groundBlendWeights(const GroundWeights& painted, const GroundWeights& heights, float blendDepth) {
    float highest = -1.0f;
    float paintedTotal = 0.0f;

    for (std::size_t layer = 0; layer < painted.size(); ++layer) {
        if (painted[layer] > 0.0f) {
            highest = std::max(highest, heights[layer]);
            paintedTotal += painted[layer];
        }
    }

    if (!(paintedTotal > 0.0f)) {
        return GroundWeights{1.0f, 0.0f, 0.0f, 0.0f};
    }

    GroundWeights shares{};
    float total = 0.0f;

    for (std::size_t layer = 0; layer < painted.size(); ++layer) {
        if (painted[layer] > 0.0f) {
            shares[layer] = painted[layer] * std::max(heights[layer] - (highest - blendDepth), 0.0f);
            total += shares[layer];
        }
    }

    const GroundWeights& source = total > 0.0f ? shares : painted;
    const float sum = total > 0.0f ? total : paintedTotal;
    GroundWeights result{};

    for (std::size_t layer = 0; layer < painted.size(); ++layer) {
        result[layer] = std::max(source[layer], 0.0f) / sum;
    }

    return result;
}

namespace {
    // One axis of groundPaintTaps: (first, second, first's share) for a place in texels.
    std::array<float, 3> paintTapsAlong(float place) {
        const float centred = place - 0.5f;
        const float cell = std::floor(centred);
        const float f = centred - cell;
        const float f2 = f * f;
        const float f3 = f2 * f;
        const float w0 = (1.0f - f) * (1.0f - f) * (1.0f - f) / 6.0f;
        const float w1 = (3.0f * f3 - 6.0f * f2 + 4.0f) / 6.0f;
        const float w2 = (-3.0f * f3 + 3.0f * f2 + 3.0f * f + 1.0f) / 6.0f;
        const float w3 = f3 / 6.0f;
        const float firstShare = w0 + w1;
        const float secondShare = w2 + w3;
        return {cell - 1.0f + w1 / firstShare + 0.5f, cell + 1.0f + w3 / secondShare + 0.5f, firstShare};
    }
}

GroundPaintTaps groundPaintTaps(Vec2 texelPlace) {
    const std::array<float, 3> alongX = paintTapsAlong(texelPlace.x);
    const std::array<float, 3> alongY = paintTapsAlong(texelPlace.y);
    return GroundPaintTaps{Vec2{alongX[0], alongY[0]}, Vec2{alongX[1], alongY[1]}, Vec2{alongX[2], alongY[2]}};
}

/*
* Mirrored from triangle.vert: the columns of the model matrix's 3 x 3 part are R's columns stretched by S, so their
* lengths are the scale and dividing by them leaves the rotation. A column squashed to nothing keeps its length (0) and
* a zero axis.
*/
ProjectionFrame projectionFrame(const Mat4& model) {
    ProjectionFrame frame{};
    float* scales[3] = {&frame.scale.x, &frame.scale.y, &frame.scale.z};

    for (int column = 0; column < 3; ++column) {
        const Vec3 axis{model.values[0][column], model.values[1][column], model.values[2][column]};
        const float length = axis.length();
        *scales[column] = length;
        frame.axes[static_cast<std::size_t>(column)] = length > 0.0f ? axis * (1.0f / length) : Vec3{0.0f, 0.0f, 0.0f};
    }

    return frame;
}

Vec3 projectionWeights(const Vec3& frameNormal) {
    const Vec3 squared = frameNormal * frameNormal;
    const Vec3 fourth = squared * squared;
    const float total = fourth.x + fourth.y + fourth.z;

    if (!(total > 0.0f)) {
        return Vec3{0.0f, 1.0f, 0.0f};
    }

    const Vec3 kept{std::max(fourth.x / total - groundProjectionSmallest, 0.0f), std::max(fourth.y / total - groundProjectionSmallest, 0.0f),
                    std::max(fourth.z / total - groundProjectionSmallest, 0.0f)};
    return kept * (1.0f / (kept.x + kept.y + kept.z));
}

Vec2 projectedPlace(const Vec3& framePosition, int axis) {
    if (axis == 0) {
        return Vec2{framePosition.z, framePosition.y};
    }

    if (axis == 1) {
        return Vec2{framePosition.x, framePosition.z};
    }

    return Vec2{framePosition.x, framePosition.y};
}

/*
* For each projection, its plane's two directions and its axis as the tangent frame: the whiteout blend works in that
* frame, with the surface's normal written in it (its parts along the plane, then its part along the axis, kept positive
* so the blend's outward part is), and the result turned back, the axis's sign restored.
*/
Vec3 projectedNormal(const Vec3& frameNormal, const std::array<Vec3, 3>& tilts, const Vec3& weights) {
    const auto whiteout = [](const Vec3& tilt, float along, float up, float out) {
        return Vec3{tilt.x + along, tilt.y + up, tilt.z * std::fabs(out)};
    };
    const auto sign = [](float value) {
        return value < 0.0f ? -1.0f : 1.0f;
    };

    const Vec3 x = whiteout(tilts[0], frameNormal.z, frameNormal.y, frameNormal.x);
    const Vec3 y = whiteout(tilts[1], frameNormal.x, frameNormal.z, frameNormal.y);
    const Vec3 z = whiteout(tilts[2], frameNormal.x, frameNormal.y, frameNormal.z);
    const Vec3 combined = Vec3{x.z * sign(frameNormal.x), x.y, x.x} * weights.x + Vec3{y.x, y.z * sign(frameNormal.y), y.y} * weights.y +
                          Vec3{z.x, z.y, z.z * sign(frameNormal.z)} * weights.z;
    const float length = combined.length();
    return length > 0.0f ? combined * (1.0f / length) : Vec3{0.0f, 0.0f, 0.0f};
}

float strataPlace(float height, float bottom, float inverseSpan) {
    return (height - bottom) * inverseSpan;
}

GroundWeights strataPaint(const GroundWeights& strata, int topLayer, float upward, float topStart, float topFull) {
    if (topLayer < 0 || topLayer > 3) {
        return strata;
    }

    const float top = topFull > topStart ? smoothStep(topStart, topFull, upward) : (upward >= topStart ? 1.0f : 0.0f);
    GroundWeights result{};

    for (std::size_t layer = 0; layer < strata.size(); ++layer) {
        result[layer] = strata[layer] * (1.0f - top);
    }

    result[static_cast<std::size_t>(topLayer)] += top;
    return result;
}

float warpedStrataHeight(float height, float warpTexel, float amplitude) {
    return height + (2.0f * warpTexel - 1.0f) * amplitude;
}

Vec2 streakPlace(const Vec3& framePosition, int axis, float inverseWidth, float inverseLength) {
    return Vec2{projectedPlace(framePosition, axis).x * inverseWidth, framePosition.y * inverseLength};
}

float streakShade(float streakTexel, float strength, float sideShare) {
    return 1.0f - strength * sideShare * (1.0f - streakTexel);
}
