#pragma once

#include "scene/twig_set.h"

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
