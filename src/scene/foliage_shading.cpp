#include "scene/foliage_shading.h"

#include "core/hash.h"

#include <algorithm>
#include <cmath>

// The share keeps clusters catching light one by one; the lean gives each clump its lit top and shaded underside.
Vec3 bentFoliageNormal(const Vec3& facing, const Vec3& lean) {
    const Vec3 blend = (facing * foliageFacingShare + lean).normalized();
    const float front = blend.dot(facing);
    return front >= foliageLeastFrontShare ? blend : (blend + facing * (foliageLeastFrontShare - front)).normalized();
}

// Between two unit normals by blend, made unit again (they are never opposite: the bent one leans to the facing).
Vec3 blendedFoliageNormal(const Vec3& normal, const Vec3& lean, float blend) {
    const Vec3 bent = bentFoliageNormal(normal, lean);
    return (normal * (1.0f - blend) + bent * blend).normalized();
}

// The use mixed into the leaf's word (a twig has a few dozen leaves), so no other hash of the same numbers agrees.
std::uint32_t leafToneSeed(std::uint32_t plant, std::uint32_t twig, std::uint32_t leaf) {
    return pcg3d({plant, twig, leaf ^ (LEAF_TONE_HASH_USE << 16u)})[0] % leafToneSeeds;
}

float leafTone(std::uint32_t seed) {
    return leafDarkestTone + leafToneRange * static_cast<float>(seed % leafToneSteps) / static_cast<float>(leafToneSteps - 1);
}

float leafWarmth(std::uint32_t seed) {
    return leafMostWarmth * (2.0f * static_cast<float>(seed / leafToneSteps % leafToneSteps) / static_cast<float>(leafToneSteps - 1) - 1.0f);
}

Vec3 leafToneScale(std::uint32_t seed) {
    const float tone = leafTone(seed);
    const float warmth = leafWarmth(seed);
    const auto linear = [](float scale) { return std::pow(scale, static_cast<float>(LEAF_TONE_GAMMA)); };
    return Vec3{linear(tone * (1.0f + warmth)), linear(tone), linear(tone * (1.0f - warmth))};
}

float numberedLeafU(float u, std::uint32_t leaf) {
    return u + static_cast<float>(leaf);
}

float tonedLeafU(float numberedU, std::uint32_t plant, std::uint32_t twig) {
    const float leaf = std::floor(numberedU);
    return numberedU - leaf + static_cast<float>(leafToneSeed(plant, twig, static_cast<std::uint32_t>(leaf)));
}

std::uint32_t leafToneSeedOf(float u) {
    return static_cast<std::uint32_t>(std::max(std::floor(u), 0.0f)) % leafToneSeeds;
}
