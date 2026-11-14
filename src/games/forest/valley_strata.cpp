#include "games/forest/valley_strata.h"

#include "core/hash.h"
#include "math/smooth_step.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
    constexpr std::uint64_t strataSeed = 0x5354'5241ull;
    constexpr std::uint64_t warpSeed = 0x5741'5250ull;
    constexpr std::uint64_t streakSeed = 0x5354'524bull;

    // The beds' thicknesses and how often one is hard.
    constexpr float thinnestSoftBed = 15.0f;
    constexpr float thickestSoftExtra = 45.0f;
    constexpr float thinnestHardBed = 5.0f;
    constexpr float thickestHardExtra = 10.0f;
    constexpr float hardChance = 0.22f;
    constexpr float smallestToneStep = 0.3f;

    float unitRandom(std::uint64_t seed, std::uint64_t index) {
        return static_cast<float>(mixBits(seed ^ mixBits(index + 0x9e3779b97f4a7c15ull)) >> 40) * (1.0f / 16777216.0f);
    }

    /*
    * Value noise that repeats every period cells: random values at the corners of a grid wrapped round (cell period is
    * cell 0 again), blended by smoothstep in between. In and out in cells; 0..1.
    */
    float periodicNoise(float x, float y, int period, std::uint64_t seed) {
        const float cellX = std::floor(x);
        const float cellY = std::floor(y);
        const float fx = smoothStep(0.0f, 1.0f, x - cellX);
        const float fy = smoothStep(0.0f, 1.0f, y - cellY);
        const auto corner = [&](int dx, int dy) {
            const int cx = ((static_cast<int>(cellX) + dx) % period + period) % period;
            const int cy = ((static_cast<int>(cellY) + dy) % period + period) % period;
            return unitRandom(seed, static_cast<std::uint64_t>(cy) * 4096u + static_cast<std::uint64_t>(cx));
        };
        const float bottom = corner(0, 0) + (corner(1, 0) - corner(0, 0)) * fx;
        const float top = corner(0, 1) + (corner(1, 1) - corner(0, 1)) * fx;
        return bottom + (top - bottom) * fy;
    }

    /*
    * A seamless square of noise, octaves of periodicNoise each with twice the cells of the last and its own seed, their
    * weights halving, normalised to 0..1, then shaped into a byte for each texel's red channel (green and blue the same,
    * alpha full).
    */
    template <typename Shape>
    ImageData seamlessNoiseImage(int size, int firstCells, int octaves, std::uint64_t seed, Shape shape) {
        ImageData image;
        image.width = static_cast<std::uint32_t>(size);
        image.height = static_cast<std::uint32_t>(size);
        image.encoding = TextureEncoding::Linear;
        image.pixels.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);

        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float sum = 0.0f;
                float weights = 0.0f;
                float weight = 1.0f;
                int cells = firstCells;

                for (int octave = 0; octave < octaves; ++octave) {
                    const float scale = static_cast<float>(cells) / static_cast<float>(size);
                    sum += weight * periodicNoise(static_cast<float>(x) * scale, static_cast<float>(y) * scale, cells, seed + static_cast<std::uint64_t>(octave));
                    weights += weight;
                    weight *= 0.5f;
                    cells *= 2;
                }

                const auto value = static_cast<std::uint8_t>(std::lround(std::clamp(shape(sum / weights), 0.0f, 1.0f) * 255.0f));
                const std::size_t texel = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)) * 4;
                image.pixels[texel] = value;
                image.pixels[texel + 1] = value;
                image.pixels[texel + 2] = value;
                image.pixels[texel + 3] = 255;
            }
        }

        return image;
    }
}

std::vector<StrataBand> valleyStrata() {
    std::vector<StrataBand> bands;
    float bottom = valleyStrataBottom;
    bool belowHard = false;
    float softTone = -1.0f;

    for (std::uint64_t bed = 0; bottom < valleyStrataTop; ++bed) {
        const float thickness = unitRandom(strataSeed, 3 * bed);
        const bool hard = !belowHard && unitRandom(strataSeed, 3 * bed + 1) < hardChance;
        StrataBand band{.bottom = bottom, .hard = hard};

        if (hard) {
            band.top = bottom + thinnestHardBed + thickestHardExtra * thickness;
        } else {
            band.top = bottom + thinnestSoftBed + thickestSoftExtra * thickness * thickness;
            const float random = unitRandom(strataSeed, 3 * bed + 2);

            // The first anywhere; then a step of 0.3 to 0.7 round the range from the soft bed below, wrapping past 1,
            // so the two always differ by at least 0.3 (the step, or one less it when it wraps).
            float tone = random;

            if (softTone >= 0.0f) {
                tone = softTone + smallestToneStep + (1.0f - 2.0f * smallestToneStep) * random;
                tone = tone >= 1.0f ? tone - 1.0f : tone;
            }

            band.tone = tone;
            softTone = tone;
        }

        bottom = band.top;
        band.top = std::min(band.top, valleyStrataTop);
        belowHard = hard;
        bands.push_back(band);
    }

    return bands;
}

const StrataBand& strataBandAt(const std::vector<StrataBand>& bands, float height) {
    for (const StrataBand& band : bands) {
        if (height < band.top) {
            return band;
        }
    }

    return bands.back();
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
            const float share = (std::min(high, band.top) - std::max(low, band.bottom)) / texelMetres;

            if (share <= 0.0f) {
                continue;
            }

            if (band.hard) {
                cover[2] += share;
            } else {
                cover[0] += share * (1.0f - band.tone);
                cover[1] += share * band.tone;
            }
        }

        for (std::size_t channel = 0; channel < 4; ++channel) {
            image.pixels[static_cast<std::size_t>(texel) * 4 + channel] = static_cast<std::uint8_t>(std::lround(std::clamp(cover[channel], 0.0f, 1.0f) * 255.0f));
        }
    }

    return image;
}

// Four octaves from 4 cells across: the beds rise and fall over a few hundred metres, with smaller wobbles on top. The
// sum of octaves bunches about the middle, so it is stretched to reach most of the range.
ImageData strataWarpImage(int size) {
    return seamlessNoiseImage(size, 4, 4, warpSeed, [](float noise) {
        return 0.5f + (noise - 0.5f) * 2.2f;
    });
}

// Clean where the noise is low; a stain darkening towards its peaks above 0.55, the darkest 0.15 on.
ImageData streakImage(int size) {
    return seamlessNoiseImage(size, 8, 3, streakSeed, [](float noise) {
        return 1.0f - smoothStep(0.55f, 0.75f, noise) * 0.85f;
    });
}
