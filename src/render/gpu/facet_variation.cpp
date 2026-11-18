#include "render/gpu/facet_variation.h"

#include <algorithm>

float facetToneScale(Vec2 facetNumbers, float amount) {
    return std::max(1.0f + amount * (2.0f * facetNumbers.x - 1.0f), 0.0f);
}

float facetRoughness(float roughness, Vec2 facetNumbers, float amount) {
    return std::clamp(roughness + amount * (2.0f * facetNumbers.y - 1.0f), 0.0f, 1.0f);
}
