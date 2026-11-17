#include "scene/card_thinning.h"
#include "math/smooth_step.h"

#include <algorithm>
#include <cmath>

// Mirrored in card_thinning.glsl.
float thinningError(const CardThinning& thinning, float share) {
    return thinning.cardSize * (std::pow(share, -thinning.growth) - 1.0f);
}

// cardSize (share^-growth - 1) = error, so share = (1 + error / cardSize)^(-1 / growth). Mirrored in card_thinning.glsl.
float thinnedShare(const CardThinning& thinning, float errorMetres) {
    if (thinning.cardSize <= 0.0f || thinning.growth <= 0.0f || errorMetres <= 0.0f) {
        return 1.0f;
    }

    const float share = std::pow(1.0f + errorMetres / thinning.cardSize, -1.0f / thinning.growth);
    return std::clamp(share, std::min(thinning.leastShare, 1.0f), 1.0f);
}

// Mirrored in card_thinning.glsl.
float cardPresence(float rank, float share) {
    return rank <= 0.0f ? 1.0f : smoothStep(rank - cardThinningBand, rank, share);
}

// Mirrored in card_thinning.glsl.
float thinnedCardScale(const CardThinning& thinning, float rank, float share) {
    return std::pow(share, -thinning.growth) * cardPresence(rank, share);
}

// Mirrored in card_thinning.glsl.
Vec3 thinnedPosition(const Vec3& position, const ThinningCard& card, float scale) {
    return card.anchor + (position - card.anchor) * scale;
}

float removedCardRank(float unitRandom, float leastShare) {
    const float lowest = std::min(leastShare + cardThinningBand, 1.0f);
    return lowest + (1.0f - lowest) * std::clamp(unitRandom, 0.0f, 1.0f);
}
