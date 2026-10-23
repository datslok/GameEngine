#include "core/coverage_mips.h"
#include "core/srgb.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace {
    using AlphaCounts = std::array<std::uint32_t, 256>;

    // How many texels have each alpha byte: enough to count coverage at any scale without walking the texels again.
    AlphaCounts countAlphas(const ImageLevel& level) {
        AlphaCounts counts{};

        for (std::size_t alpha = 3; alpha < level.pixels.size(); alpha += 4) {
            ++counts[level.pixels[alpha]];
        }

        return counts;
    }

    std::uint8_t scaledAlpha(std::uint32_t alpha, float scale) {
        return static_cast<std::uint8_t>(std::min(std::lround(static_cast<float>(alpha) * scale), 255l));
    }

    // The coverage once every alpha is multiplied by scale (and rounded to a byte, as it will be stored).
    float scaledCoverage(const AlphaCounts& counts, std::uint32_t texels, float scale, float cutoff) {
        std::uint32_t kept = 0;

        for (std::uint32_t alpha = 0; alpha < counts.size(); ++alpha) {
            if (static_cast<float>(scaledAlpha(alpha, scale)) / 255.0f >= cutoff) {
                kept += counts[alpha];
            }
        }

        return static_cast<float>(kept) / static_cast<float>(texels);
    }

    // The largest scale tried: enough to lift a single 1 to the top byte.
    constexpr float largestScale = 255.0f;
    constexpr int bisectionSteps = 32;

    /*
    * Coverage only grows with the scale, in steps (the byte rounding), so the scale is found by halving an interval:
    * the coverage nearest the target, then the range of scales that give exactly it, and the one in that range nearest
    * 1, so a level that already has the right coverage keeps its alpha as averaged (solid stays solid).
    */
    float coverageScale(const AlphaCounts& counts, std::uint32_t texels, float target, float cutoff) {
        const auto coverage = [&](float scale) { return scaledCoverage(counts, texels, scale, cutoff); };

        // below: the largest scale found under the target; above: the smallest found reaching it.
        float below = 0.0f;
        float above = largestScale;

        for (int step = 0; step < bisectionSteps; ++step) {
            const float middle = (below + above) * 0.5f;
            (coverage(middle) < target ? below : above) = middle;
        }

        const float nearest = std::abs(coverage(below) - target) < std::abs(coverage(above) - target) ? coverage(below) : coverage(above);

        // The first scale giving the nearest coverage, and the last.
        float low = 0.0f;
        float high = largestScale;

        for (int step = 0; step < bisectionSteps; ++step) {
            const float middle = (low + high) * 0.5f;
            (coverage(middle) < nearest ? low : high) = middle;
        }

        const float first = high;
        low = first;
        high = largestScale;

        if (coverage(largestScale) <= nearest) {
            low = largestScale;
        } else {
            for (int step = 0; step < bisectionSteps; ++step) {
                const float middle = (low + high) * 0.5f;
                (coverage(middle) <= nearest ? low : high) = middle;
            }
        }

        return std::clamp(1.0f, first, low);
    }
}

float alphaCoverage(const ImageLevel& level, float cutoff) {
    const std::uint32_t texels = level.width * level.height;

    if (texels == 0) {
        throw std::invalid_argument("alphaCoverage: an empty image has no coverage");
    }

    return scaledCoverage(countAlphas(level), texels, 1.0f, cutoff);
}

ImageLevel halveImage(const ImageLevel& level, bool srgb) {
    if (level.width == 0 || level.height == 0 || level.pixels.size() != std::size_t{level.width} * level.height * 4) {
        throw std::invalid_argument("halveImage: the image needs four bytes for each of its texels");
    }

    ImageLevel half;
    half.width = std::max(level.width / 2, 1u);
    half.height = std::max(level.height / 2, 1u);
    half.pixels.resize(std::size_t{half.width} * half.height * 4);

    for (std::uint32_t y = 0; y < half.height; ++y) {
        // The last row of the half takes the odd row left over below it too.
        const std::uint32_t firstRow = y * 2;
        const std::uint32_t lastRow = y + 1 == half.height ? level.height - 1 : std::min(firstRow + 1, level.height - 1);

        for (std::uint32_t x = 0; x < half.width; ++x) {
            const std::uint32_t firstColumn = x * 2;
            const std::uint32_t lastColumn = x + 1 == half.width ? level.width - 1 : std::min(firstColumn + 1, level.width - 1);
            float sums[4]{};

            for (std::uint32_t row = firstRow; row <= lastRow; ++row) {
                for (std::uint32_t column = firstColumn; column <= lastColumn; ++column) {
                    const std::uint8_t* texel = &level.pixels[(std::size_t{row} * level.width + column) * 4];

                    for (int channel = 0; channel < 4; ++channel) {
                        sums[channel] += srgb && channel < 3 ? srgbByteToLinear(texel[channel]) : static_cast<float>(texel[channel]);
                    }
                }
            }

            const float count = static_cast<float>((lastRow - firstRow + 1) * (lastColumn - firstColumn + 1));
            std::uint8_t* out = &half.pixels[(std::size_t{y} * half.width + x) * 4];

            for (int channel = 0; channel < 4; ++channel) {
                const float mean = sums[channel] / count;
                out[channel] = srgb && channel < 3 ? linearToSrgbByte(mean) : static_cast<std::uint8_t>(std::lround(mean));
            }
        }
    }

    return half;
}

/*
* Each level is halved from the last one as averaged, before its scaling, so scales do not compound down the chain;
* then its alpha alone is scaled.
*/
std::vector<ImageLevel> coveragePreservingMips(const ImageLevel& full, bool srgb, float cutoff) {
    const float target = alphaCoverage(full, cutoff);
    std::vector<ImageLevel> levels;
    ImageLevel averaged = full;

    while (averaged.width > 1 || averaged.height > 1) {
        averaged = halveImage(averaged, srgb);
        ImageLevel scaled = averaged;
        const float scale = coverageScale(countAlphas(averaged), averaged.width * averaged.height, target, cutoff);

        for (std::size_t alpha = 3; alpha < scaled.pixels.size(); alpha += 4) {
            scaled.pixels[alpha] = scaledAlpha(scaled.pixels[alpha], scale);
        }

        levels.push_back(std::move(scaled));
    }

    return levels;
}
