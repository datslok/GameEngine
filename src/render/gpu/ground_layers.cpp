#include "render/gpu/ground_layers.h"

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
