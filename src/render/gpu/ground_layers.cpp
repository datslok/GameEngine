#include "render/gpu/ground_layers.h"

#include <algorithm>

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
