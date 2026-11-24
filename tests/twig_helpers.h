#pragma once

#include "scene/twig_set.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

// Twigs scattered through a crown 20 m across, each turned at random, a few prototypes (index modulo prototypes), some
// larger.
inline std::vector<TwigInstance> scatteredTwigs(std::size_t count, std::uint32_t prototypes = 3) {
    std::mt19937 random(5u);
    std::uniform_real_distribution<float> place(-10.0f, 10.0f);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::vector<TwigInstance> twigs;

    for (std::size_t index = 0; index < count; ++index) {
        Vec3 facing{unit(random), unit(random), unit(random)};
        facing = facing.lengthSquared() > 1e-4f ? facing.normalized() : Vec3{0.0f, 0.0f, 1.0f};
        const Vec3 helper = std::abs(facing.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
        const Vec3 across = helper.cross(facing).normalized();
        const Vec3 towardsTip = facing.cross(across);
        twigs.push_back(TwigInstance{.foot = Vec3{place(random), 15.0f + place(random) * 0.5f, place(random)},
                                     .across = across,
                                     .towardsTip = towardsTip,
                                     .facing = facing,
                                     .scale = index % 7 == 0 ? 1.5f : 1.0f,
                                     .prototype = static_cast<std::uint32_t>(index % prototypes),
                                     .lean = Vec3{0.1f, 0.2f, 0.0f}});
    }

    return twigs;
}

// Three prototypes handing over 4, 5 and 3 far leaves of different sides, areas and reaches, grown 2, 1.8 and 2.2 times
// over their finest sides (stand-ins for the forest's twigFarLeaves).
inline std::vector<TwigFarLeaves> standInFarLeaves() {
    std::vector<TwigFarLeaves> prototypes;
    const std::uint32_t counts[3] = {4, 5, 3};
    const float growths[3] = {2.0f, 1.8f, 2.2f};

    for (std::uint32_t prototype = 0; prototype < 3; ++prototype) {
        TwigFarLeaves& made = prototypes.emplace_back();
        made.growth = growths[prototype];

        for (std::uint32_t leaf = 0; leaf < counts[prototype]; ++leaf) {
            const float turn = 0.7f * static_cast<float>(leaf + prototype);
            const Vec3 towardsTip{-std::sin(turn), std::cos(turn), 0.0f};
            made.leaves.push_back(TwigLeafPlace{.foot = Vec3{0.02f * static_cast<float>(leaf), 0.05f * static_cast<float>(leaf + 1), 0.0f},
                                                .towardsTip = towardsTip,
                                                .facing = Vec3{0.0f, 0.0f, 1.0f},
                                                .side = 0.06f + 0.01f * static_cast<float>(leaf),
                                                .area = 0.3f + 0.05f * static_cast<float>(prototype),
                                                .reach = 1.05f,
                                                .cell = (leaf * 5 + prototype) % 24,
                                                .number = leaf * 3 + 1});
            made.error = std::max(made.error, made.leaves.back().side / made.growth * (made.growth - 1.0f));
        }
    }

    return prototypes;
}

// The 24 cells the stand-in far leaves name, each a slightly different shape, folded and curled.
inline FarLeafShapes standInFarShapes() {
    FarLeafShapes shapes{.cells = {}, .foldRadians = 0.25f, .curl = 0.08f};

    for (int cell = 0; cell < 24; ++cell) {
        const float step = static_cast<float>(cell) * 0.01f;
        shapes.cells.push_back(FarLeafShape{.base = 0.05f + step,
                                            .tip = 0.95f - step,
                                            .up = 0.5f + step,
                                            .left = 0.3f - step,
                                            .right = 0.28f,
                                            .uvMin = Vec2{static_cast<float>(cell % 6) / 6.0f, static_cast<float>(cell / 6) / 4.0f},
                                            .uvMax = Vec2{static_cast<float>(cell % 6 + 1) / 6.0f, static_cast<float>(cell / 6 + 1) / 4.0f}});
    }

    return shapes;
}
