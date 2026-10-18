#include "render/gpu/bloom.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

void validateBloom(const BloomSettings& settings) {
    if (!(settings.strength >= 0.0f && settings.strength <= 1.0f)) {
        throw std::invalid_argument("Bloom strength must be between 0 and 1");
    }

    if (!(settings.radius > 0.0f)) {
        throw std::invalid_argument("Bloom radius must be positive");
    }

    if (settings.maxLevels < 1 || settings.maxLevels > BLOOM_LEVEL_LIMIT) {
        throw std::invalid_argument("Bloom must have between 1 and BLOOM_LEVEL_LIMIT levels");
    }
}

/*
* Level k is the frame halved k + 1 times, so its shorter side is the frame's shifted right k + 1 places. The loop ends
* well before a shift could pass the integer's width, since the side drops below the limit first.
*/
std::uint32_t bloomLevelCount(std::uint32_t frameWidth, std::uint32_t frameHeight, std::uint32_t maxLevels) {
    const std::uint32_t shortSide = std::min(frameWidth, frameHeight);
    std::uint32_t count = 0;

    while (count < maxLevels && (shortSide >> (count + 1)) >= BLOOM_SMALLEST_LEVEL_SIDE) {
        ++count;
    }

    return count;
}

// Never below one texel, so even a level past the chain's end is a real image.
BloomLevelSize bloomLevelSize(std::uint32_t frameWidth, std::uint32_t frameHeight, std::uint32_t level) {
    const std::uint32_t shift = std::min(level + 1, 31u);

    return BloomLevelSize{
        std::max(frameWidth >> shift, 1u),
        std::max(frameHeight >> shift, 1u)
    };
}

BloomImage::BloomImage(std::uint32_t width, std::uint32_t height, const Vec3& fill):
    width(width),
    height(height),
    pixels(static_cast<std::size_t>(width) * height, fill)
{
}

Vec3& BloomImage::at(std::uint32_t x, std::uint32_t y) {
    return pixels[static_cast<std::size_t>(y) * width + x];
}

const Vec3& BloomImage::at(std::uint32_t x, std::uint32_t y) const {
    return pixels[static_cast<std::size_t>(y) * width + x];
}

namespace {
    // The texel at a column or row index, clamped into the image, as the sampler's clamp-to-edge mode does.
    std::uint32_t clampIndex(float index, std::uint32_t size) {
        return static_cast<std::uint32_t>(std::clamp(index, 0.0f, static_cast<float>(size - 1)));
    }
}

/*
* In texel units the centres sit at whole numbers after shifting by half a texel, so the integer part picks the texel
* before the point and the fraction is how far it is towards the next one.
*/
Vec3 sampleBilinear(const BloomImage& image, float u, float v) {
    const float x = u * static_cast<float>(image.width) - 0.5f;
    const float y = v * static_cast<float>(image.height) - 0.5f;
    const float left = std::floor(x);
    const float top = std::floor(y);
    const float across = x - left;
    const float down = y - top;

    const std::uint32_t x0 = clampIndex(left, image.width);
    const std::uint32_t x1 = clampIndex(left + 1.0f, image.width);
    const std::uint32_t y0 = clampIndex(top, image.height);
    const std::uint32_t y1 = clampIndex(top + 1.0f, image.height);

    const Vec3 upper = image.at(x0, y0) * (1.0f - across) + image.at(x1, y0) * across;
    const Vec3 lower = image.at(x0, y1) * (1.0f - across) + image.at(x1, y1) * across;
    return upper * (1.0f - down) + lower * down;
}

float karisWeight(const Vec3& colour) {
    const float luminance = static_cast<float>(BLOOM_LUMA_RED) * colour.x +
                            static_cast<float>(BLOOM_LUMA_GREEN) * colour.y +
                            static_cast<float>(BLOOM_LUMA_BLUE) * colour.z;
    return 1.0f / (1.0f + luminance);
}

/*
* Thirteen bilinear reads around each output texel's centre: a 3x3 grid two source texels apart and four reads one
* texel diagonally out. Every read lands where four source texels meet, so each is the average of a 2x2 block, and the
* 13 reads cover a 6x6 patch. They form five 4x4 boxes (four reads each): the inner box of the diagonal reads, and the
* four corner boxes of the grid. Averaging overlapping boxes, instead of one 2x2 block per output texel, means a thin
* bright line moving by a pixel changes the result smoothly instead of jumping between one and two texels (aliasing).
*/
BloomImage bloomDownsample(const BloomImage& source, BloomLevelSize size, bool karis) {
    if (size.width == 0 || size.height == 0 || source.width == 0 || source.height == 0) {
        throw std::invalid_argument("A bloom downsample needs non-empty images");
    }

    const float sourceTexelX = 1.0f / static_cast<float>(source.width);
    const float sourceTexelY = 1.0f / static_cast<float>(source.height);
    const float innerWeight = static_cast<float>(BLOOM_INNER_BOX_WEIGHT);
    const float outerWeight = static_cast<float>(BLOOM_OUTER_BOX_WEIGHT);

    BloomImage result(size.width, size.height);

    for (std::uint32_t row = 0; row < size.height; ++row) {
        for (std::uint32_t column = 0; column < size.width; ++column) {
            const float u = (static_cast<float>(column) + 0.5f) / static_cast<float>(size.width);
            const float v = (static_cast<float>(row) + 0.5f) / static_cast<float>(size.height);

            // Nothing below black, so a stray negative value cannot spread.
            const auto read = [&](float texelsX, float texelsY) {
                const Vec3 colour = sampleBilinear(source, u + texelsX * sourceTexelX, v + texelsY * sourceTexelY);
                return Vec3{std::max(colour.x, 0.0f), std::max(colour.y, 0.0f), std::max(colour.z, 0.0f)};
            };

            const Vec3 a = read(-2.0f, -2.0f);
            const Vec3 b = read(0.0f, -2.0f);
            const Vec3 c = read(2.0f, -2.0f);
            const Vec3 d = read(-2.0f, 0.0f);
            const Vec3 e = read(0.0f, 0.0f);
            const Vec3 f = read(2.0f, 0.0f);
            const Vec3 g = read(-2.0f, 2.0f);
            const Vec3 h = read(0.0f, 2.0f);
            const Vec3 i = read(2.0f, 2.0f);
            const Vec3 j = read(-1.0f, -1.0f);
            const Vec3 k = read(1.0f, -1.0f);
            const Vec3 l = read(-1.0f, 1.0f);
            const Vec3 m = read(1.0f, 1.0f);

            const Vec3 boxes[5] = {
                (j + k + l + m) * 0.25f,
                (a + b + d + e) * 0.25f,
                (b + c + e + f) * 0.25f,
                (d + e + g + h) * 0.25f,
                (e + f + h + i) * 0.25f
            };
            const float weights[5] = {innerWeight, outerWeight, outerWeight, outerWeight, outerWeight};

            Vec3 sum{};
            float weightSum = 0.0f;

            for (int box = 0; box < 5; ++box) {
                const float weight = weights[box] * (karis ? karisWeight(boxes[box]) : 1.0f);
                sum = sum + boxes[box] * weight;
                weightSum += weight;
            }

            result.at(column, row) = sum / weightSum;
        }
    }

    return result;
}

/*
* Nine bilinear reads of the smaller level in a 3x3 grid `radius` of its texels apart, weighted 1-2-1 by 2-4-2 by 1-2-1
* (a tent: the bilinear reads are tents themselves, so the result is smooth, with no blocky texel edges).
*/
void bloomUpsampleAdd(BloomImage& target, const BloomImage& smaller, float radius) {
    const float stepX = radius / static_cast<float>(smaller.width);
    const float stepY = radius / static_cast<float>(smaller.height);
    const float corner = static_cast<float>(BLOOM_TENT_CORNER);
    const float edge = static_cast<float>(BLOOM_TENT_EDGE);
    const float centre = static_cast<float>(BLOOM_TENT_CENTRE);

    for (std::uint32_t row = 0; row < target.height; ++row) {
        for (std::uint32_t column = 0; column < target.width; ++column) {
            const float u = (static_cast<float>(column) + 0.5f) / static_cast<float>(target.width);
            const float v = (static_cast<float>(row) + 0.5f) / static_cast<float>(target.height);

            const auto read = [&](float stepsX, float stepsY) {
                return sampleBilinear(smaller, u + stepsX * stepX, v + stepsY * stepY);
            };

            const Vec3 corners = read(-1.0f, -1.0f) + read(1.0f, -1.0f) + read(-1.0f, 1.0f) + read(1.0f, 1.0f);
            const Vec3 edges = read(0.0f, -1.0f) + read(-1.0f, 0.0f) + read(1.0f, 0.0f) + read(0.0f, 1.0f);

            target.at(column, row) = target.at(column, row) + corners * corner + edges * edge + read(0.0f, 0.0f) * centre;
        }
    }
}

/*
* Down the chain, then back up: each level, from the second smallest, adds the grown level below it, so level 0 ends up
* as the sum of every level's blur. Each of those holds all the light once, so dividing by their number keeps the total.
*/
BloomImage bloomBlur(const BloomImage& scene, const BloomSettings& settings) {
    validateBloom(settings);

    const std::uint32_t levelCount = bloomLevelCount(scene.width, scene.height, settings.maxLevels);

    if (levelCount == 0) {
        return BloomImage{};
    }

    std::vector<BloomImage> levels;
    levels.reserve(levelCount);
    levels.push_back(bloomDownsample(scene, bloomLevelSize(scene.width, scene.height, 0), settings.fireflyReduction));

    for (std::uint32_t level = 1; level < levelCount; ++level) {
        levels.push_back(bloomDownsample(levels.back(), bloomLevelSize(scene.width, scene.height, level), false));
    }

    for (std::uint32_t level = levelCount - 1; level > 0; --level) {
        bloomUpsampleAdd(levels[level - 1], levels[level], settings.radius);
    }

    const BloomComposite composite = bloomComposite(settings, levelCount);

    for (Vec3& pixel : levels[0].pixels) {
        pixel = pixel * composite.scale;
    }

    return levels[0];
}

// Written as GLSL's mix is, so the CPU and the shader round alike.
Vec3 mixBloom(const Vec3& scene, const Vec3& bloom, float strength) {
    return scene * (1.0f - strength) + bloom * strength;
}

BloomComposite bloomComposite(const BloomSettings& settings, std::uint32_t levelCount) {
    validateBloom(settings);

    if (!settings.enabled || levelCount == 0) {
        return BloomComposite{};
    }

    return BloomComposite{settings.strength, 1.0f / static_cast<float>(levelCount)};
}

BloomPassUniformData packBloomPass(BloomLevelSize source, BloomLevelSize target, float option) {
    return BloomPassUniformData{
        {
            1.0f / static_cast<float>(source.width),
            1.0f / static_cast<float>(source.height),
            1.0f / static_cast<float>(target.width),
            1.0f / static_cast<float>(target.height)
        },
        {option, 0.0f, 0.0f, 0.0f}
    };
}
