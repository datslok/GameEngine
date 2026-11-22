#include "render/gpu/material_shared.h"
#include "render/gpu/material_uniforms.h"
#include "scene/foliage_shading.h"
#include "scene/material.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

namespace {
    bool nearVector(const Vec3& a, const Vec3& b, float tolerance) {
        return (a - b).length() <= tolerance;
    }

    // The bent normal: the facing's share plus the lean, made unit; held at the least front share when the lean pulls it
    // past square; blend 0 is the leaf's own normal, 1 the bent one, between them unit and between them.
    void testFoliageNormals() {
        const Vec3 facing{0.0f, 0.0f, 1.0f};
        const Vec3 lean{0.4f, 0.3f, 0.0f};
        const Vec3 bent = bentFoliageNormal(facing, lean);
        assert(nearVector(bent, (facing * foliageFacingShare + lean).normalized(), 1e-6f));

        const Vec3 behind = bentFoliageNormal(facing, Vec3{0.0f, 0.0f, -0.9f} + Vec3{0.2f, 0.0f, 0.0f});
        assert(behind.dot(facing) >= foliageLeastFrontShare * 0.5f && std::abs(behind.length() - 1.0f) < 1e-5f);

        assert(nearVector(blendedFoliageNormal(facing, lean, 0.0f), facing, 1e-6f));
        assert(nearVector(blendedFoliageNormal(facing, lean, 1.0f), bent, 1e-6f));
        const Vec3 half = blendedFoliageNormal(facing, lean, 0.5f);
        assert(std::abs(half.length() - 1.0f) < 1e-5f && half.dot(facing) > bent.dot(facing) && half.dot(bent) > facing.dot(bent));
    }

    // Tones: every seed within the cards' ranges, the steps reaching both ends; seeds spread over all LEAF_TONE_SEEDS for
    // a twig's leaves and differ between twigs and plants; the scale is the tone and warmth taken to linear light.
    void testLeafTones() {
        float darkest = 10.0f;
        float brightest = 0.0f;
        float coolest = 10.0f;
        float warmest = -10.0f;

        for (std::uint32_t seed = 0; seed < leafToneSeeds; ++seed) {
            darkest = std::min(darkest, leafTone(seed));
            brightest = std::max(brightest, leafTone(seed));
            coolest = std::min(coolest, leafWarmth(seed));
            warmest = std::max(warmest, leafWarmth(seed));
        }

        assert(std::abs(darkest - leafDarkestTone) < 1e-6f && std::abs(brightest - (leafDarkestTone + leafToneRange)) < 1e-6f);
        assert(std::abs(coolest + leafMostWarmth) < 1e-6f && std::abs(warmest - leafMostWarmth) < 1e-6f);

        std::set<std::uint32_t> seeds;
        std::uint32_t sameAcrossTwigs = 0;
        std::uint32_t sameAcrossPlants = 0;

        for (std::uint32_t twig = 0; twig < 64; ++twig) {
            for (std::uint32_t leaf = 0; leaf < 16; ++leaf) {
                const std::uint32_t seed = leafToneSeed(5u, twig, leaf);
                assert(seed < leafToneSeeds);
                seeds.insert(seed);
                sameAcrossTwigs += seed == leafToneSeed(5u, twig + 1, leaf) ? 1 : 0;
                sameAcrossPlants += seed == leafToneSeed(6u, twig, leaf) ? 1 : 0;
            }
        }

        assert(seeds.size() == leafToneSeeds && sameAcrossTwigs < 64 && sameAcrossPlants < 64);

        const std::uint32_t seed = 42;
        const Vec3 scale = leafToneScale(seed);
        const float tone = leafTone(seed);
        const float warmth = leafWarmth(seed);
        assert(std::abs(scale.y - std::pow(tone, 2.2f)) < 1e-5f);
        assert(std::abs(scale.x - std::pow(tone * (1.0f + warmth), 2.2f)) < 1e-5f && std::abs(scale.z - std::pow(tone * (1.0f - warmth), 2.2f)) < 1e-5f);
    }

    // A leaf's number rides the texture coordinate's whole part: the fraction (what the texture reads) is kept through
    // numbering and toning, and the fragment reads back the seed the vertex put there.
    void testLeafNumbersInTheTextureCoordinate() {
        for (std::uint32_t leaf = 0; leaf < 40; ++leaf) {
            for (const float u : {0.0f, 0.137f, 0.5f, 0.999f}) {
                const float numbered = numberedLeafU(u, leaf);
                assert(std::floor(numbered) == static_cast<float>(leaf) && std::abs(numbered - std::floor(numbered) - u) < 1e-5f);
                const float toned = tonedLeafU(numbered, 3u, 7u);
                assert(std::abs(toned - std::floor(toned) - u) < 1e-5f);
                assert(leafToneSeedOf(toned) == leafToneSeed(3u, 7u, leaf));
            }
        }

        assert(leafToneSeedOf(-0.5f) == 0u && leafToneSeedOf(static_cast<float>(leafToneSeeds) + 3.5f) == 3u);
    }

    // A material tones its leaves only when asked (the twigs' leaves), the flag packed for the fragment shader.
    void testTheLeafTonesFlag() {
        Material material;
        assert((packMaterial(material).extra[1] & MATERIAL_LEAF_TONES) == 0u);
        material.leafTones = true;
        assert((packMaterial(material).extra[1] & MATERIAL_LEAF_TONES) != 0u);
    }
}

void testFoliageShading() {
    testFoliageNormals();
    testLeafTones();
    testLeafNumbersInTheTextureCoordinate();
    testTheLeafTonesFlag();
}
