#include "games/forest/valley_strata.h"

#include "core/hash.h"

#include <algorithm>
#include <cmath>

namespace {
    constexpr std::uint64_t strataSeed = 0x5354'5241ull;
    constexpr float thinnestBed = 4.0f;
    constexpr float thickestExtra = 26.0f;

    // How often each rock is chosen, before leaving out the one below: the red-brown strata most.
    constexpr std::array<float, valleyStrataRocks> rockChances{0.5f, 0.25f, 0.25f};

    float unitRandom(std::uint64_t index) {
        return static_cast<float>(mixBits(strataSeed ^ mixBits(index + 0x9e3779b97f4a7c15ull)) >> 40) * (1.0f / 16777216.0f);
    }

    // A rock other than the one below, by the chances of the rest.
    int chooseRock(float random, int below) {
        float total = 0.0f;

        for (int rock = 0; rock < valleyStrataRocks; ++rock) {
            total += rock == below ? 0.0f : rockChances[static_cast<std::size_t>(rock)];
        }

        float reach = random * total;
        int chosen = below == 0 ? 1 : 0;

        for (int rock = 0; rock < valleyStrataRocks; ++rock) {
            if (rock == below) {
                continue;
            }

            chosen = rock;
            reach -= rockChances[static_cast<std::size_t>(rock)];

            if (reach < 0.0f) {
                break;
            }
        }

        return chosen;
    }
}

std::vector<StrataBand> valleyStrata() {
    std::vector<StrataBand> bands;
    float bottom = valleyStrataBottom;
    int below = -1;

    for (std::uint64_t bed = 0; bottom < valleyStrataTop; ++bed) {
        const float random = unitRandom(2 * bed);
        const float thickness = thinnestBed + thickestExtra * random * random;
        const int rock = chooseRock(unitRandom(2 * bed + 1), below);
        bands.push_back(StrataBand{bottom, std::min(bottom + thickness, valleyStrataTop), rock});
        bottom += thickness;
        below = rock;
    }

    return bands;
}

int strataRockAt(const std::vector<StrataBand>& bands, float height) {
    for (const StrataBand& band : bands) {
        if (height < band.top) {
            return band.rock;
        }
    }

    return bands.empty() ? 0 : bands.back().rock;
}

ImageData strataImage(const std::vector<StrataBand>& bands, int texels) {
    ImageData image;
    image.width = static_cast<std::uint32_t>(texels);
    image.height = 1;
    image.encoding = TextureEncoding::Linear;
    image.pixels.assign(static_cast<std::size_t>(texels) * 4, 0);
    const float texelMetres = (valleyStrataTop - valleyStrataBottom) / static_cast<float>(texels);

    for (int texel = 0; texel < texels; ++texel) {
        const float low = valleyStrataBottom + texelMetres * static_cast<float>(texel);
        const float high = low + texelMetres;
        std::array<float, 4> cover{};

        for (const StrataBand& band : bands) {
            const float overlap = std::min(high, band.top) - std::max(low, band.bottom);

            if (overlap > 0.0f) {
                cover[static_cast<std::size_t>(band.rock)] += overlap / texelMetres;
            }
        }

        for (std::size_t channel = 0; channel < 4; ++channel) {
            image.pixels[static_cast<std::size_t>(texel) * 4 + channel] = static_cast<std::uint8_t>(std::lround(std::clamp(cover[channel], 0.0f, 1.0f) * 255.0f));
        }
    }

    return image;
}
