#include "core/coverage_mips.h"
#include "core/hash.h"
#include "render/gpu/mip_levels.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {
    ImageLevel solidImage(std::uint32_t width, std::uint32_t height, std::uint8_t grey, std::uint8_t alpha) {
        ImageLevel image{width, height, {}};

        for (std::uint32_t texel = 0; texel < width * height; ++texel) {
            image.pixels.insert(image.pixels.end(), {grey, grey, grey, alpha});
        }

        return image;
    }

    std::uint8_t& alphaAt(ImageLevel& image, std::uint32_t x, std::uint32_t y) {
        return image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4 + 3];
    }

    // Leaves: most texels nearly clear, some solid, scattered without a pattern, so averaging pulls every small mip's
    // alpha towards the mean, under the cutoff.
    ImageLevel leafyImage(std::uint32_t size) {
        ImageLevel image = solidImage(size, size, 90, 0);

        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const std::uint32_t bits = static_cast<std::uint32_t>(mixBits(y * size + x));
                alphaAt(image, x, y) = bits % 100 < 30 ? 255 : static_cast<std::uint8_t>(bits % 60);
            }
        }

        return image;
    }

    // Halving averages each 2 x 2 block; data is averaged as stored.
    void testHalvingAveragesData() {
        ImageLevel image = solidImage(2, 2, 0, 255);
        image.pixels[0] = 200;
        alphaAt(image, 1, 1) = 55;

        const ImageLevel half = halveImage(image, false);
        assert(half.width == 1 && half.height == 1 && half.pixels.size() == 4);
        assert(half.pixels[0] == 50);
        assert(half.pixels[3] == 205);
    }

    // sRGB bytes are averaged as light: black and white make a mid grey of light, which is byte 188, not 128. Alpha is
    // always a plain share.
    void testHalvingAveragesSrgbAsLight() {
        ImageLevel image = solidImage(2, 1, 0, 0);
        image.pixels[4] = 255;
        image.pixels[5] = 255;
        image.pixels[6] = 255;
        alphaAt(image, 1, 0) = 255;

        const ImageLevel linear = halveImage(image, false);
        const ImageLevel srgb = halveImage(image, true);
        assert(linear.pixels[0] == 128);
        assert(srgb.pixels[0] == 188);
        assert(srgb.pixels[3] == 128);
    }

    // Each level is half the last, rounded down, at least one texel, as the GPU sizes mips; an odd last row or column
    // folds into its neighbour's block.
    void testOddSizesAndChainLength() {
        const ImageLevel half = halveImage(solidImage(3, 5, 40, 255), false);
        assert(half.width == 1 && half.height == 2);
        assert(half.pixels[0] == 40);

        const std::vector<ImageLevel> chain = coveragePreservingMips(solidImage(37, 9, 40, 255), false, 0.5f);
        assert(chain.size() + 1 == mipLevelCount(37, 9));
        assert(chain.back().width == 1 && chain.back().height == 1);
    }

    // The share of texels a cut-out keeps: alpha at or above the cutoff, as the shader tests it (byte / 255).
    void testCoverage() {
        ImageLevel image = solidImage(4, 1, 0, 0);
        alphaAt(image, 0, 0) = 255;
        alphaAt(image, 1, 0) = 128;
        alphaAt(image, 2, 0) = 127;
        assert(alphaCoverage(image, 0.5f) == 0.5f);
        assert(alphaCoverage(image, 0.0f) == 1.0f);
    }

    // Plain averaging loses the leaves at a distance; scaling each level's alpha keeps the share the full size shows.
    void testLeavesKeepTheirCoverage() {
        const ImageLevel full = leafyImage(64);
        const float target = alphaCoverage(full, 0.5f);
        assert(target > 0.25f && target < 0.35f);

        ImageLevel plain = full;

        for (int level = 0; level < 3; ++level) {
            plain = halveImage(plain, true);
        }

        assert(alphaCoverage(plain, 0.5f) < 0.1f);

        const std::vector<ImageLevel> chain = coveragePreservingMips(full, true, 0.5f);

        for (const ImageLevel& level : chain) {
            const float texels = static_cast<float>(level.width * level.height);

            if (texels >= 16.0f) {
                assert(std::abs(alphaCoverage(level, 0.5f) - target) <= 1.0f / texels + 0.02f);
            }
        }
    }

    // Only alpha is scaled: the colour is the plain average.
    void testColourIsUntouched() {
        const ImageLevel full = leafyImage(16);
        const std::vector<ImageLevel> chain = coveragePreservingMips(full, true, 0.5f);
        const ImageLevel half = halveImage(full, true);

        for (std::size_t byte = 0; byte < half.pixels.size(); ++byte) {
            if (byte % 4 != 3) {
                assert(chain[0].pixels[byte] == half.pixels[byte]);
            }
        }
    }

    // Solid stays solid, at full alpha (the scale nearest 1 that keeps the coverage); clear stays clear.
    void testSolidAndClearStayAsTheyAre() {
        for (const ImageLevel& level : coveragePreservingMips(solidImage(8, 8, 10, 255), false, 0.5f)) {
            assert(level.pixels[3] == 255 && alphaCoverage(level, 0.5f) == 1.0f);
        }

        for (const ImageLevel& level : coveragePreservingMips(solidImage(8, 8, 10, 0), false, 0.5f)) {
            assert(alphaCoverage(level, 0.5f) == 0.0f);
        }
    }
}

void testCoverageMips() {
    testHalvingAveragesData();
    testHalvingAveragesSrgbAsLight();
    testOddSizesAndChainLength();
    testCoverage();
    testLeavesKeepTheirCoverage();
    testColourIsUntouched();
    testSolidAndClearStayAsTheyAre();
}
