#include "core/block_compression.h"
#include "core/coverage_mips.h"
#include "core/hash.h"
#include "core/job_system.h"
#include "test_jobs.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace {
    // A smooth picture: red across, green down, alpha solid; like a photographed surface's slow shading, which BC7's
    // line of colours per block fits well. With the wave, blue rises and falls diagonally on its own: a block's colours
    // then lie far from any one line, a hard case.
    ImageLevel gradientImage(std::uint32_t width, std::uint32_t height, bool wave = true) {
        ImageLevel image{width, height, {}};

        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto red = static_cast<std::uint8_t>(x * 255 / std::max(width - 1, 1u));
                const auto green = static_cast<std::uint8_t>(y * 255 / std::max(height - 1, 1u));
                const auto blue = wave ? static_cast<std::uint8_t>(127.5 + 127.0 * std::sin(0.15 * (x + y))) : std::uint8_t{60};
                image.pixels.insert(image.pixels.end(), {red, green, blue, 255});
            }
        }

        return image;
    }

    // Peak signal to noise ratio over red, green and blue, in decibels: 10 log10(255^2 / mean squared error).
    double colourPsnr(const ImageLevel& original, const ImageLevel& decoded) {
        double squaredError = 0.0;
        std::size_t samples = 0;

        for (std::size_t index = 0; index < original.pixels.size(); ++index) {
            if (index % 4 == 3) {
                continue;
            }
            const double difference = static_cast<double>(original.pixels[index]) - static_cast<double>(decoded.pixels[index]);
            squaredError += difference * difference;
            ++samples;
        }

        const double meanSquared = squaredError / static_cast<double>(samples);
        return meanSquared == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / meanSquared);
    }

    // A level takes a 16-byte block for every 4 x 4 texels, a part-filled block at an odd edge still a whole one.
    void testLevelSizes() {
        assert(bc7LevelBytes(1024, 1024) == 1024u * 1024u);
        assert(bc7LevelBytes(4, 4) == 16);
        assert(bc7LevelBytes(5, 3) == 2 * 1 * 16);
        assert(bc7LevelBytes(1, 1) == 16);
        assert(bc7LevelBytes(1024, 2048) == 1024u * 2048u);
    }

    // A smooth picture comes back close. Measured (quality 0): 42.6 dB perceptual, 43.7 plain on the gradient; on the
    // wave 38.6 perceptual (which spends little on blue), 40.4 plain.
    void testRoundTripKeepsAGradient() {
        const ImageLevel original = gradientImage(64, 48, false);
        const CompressedLevel compressed = compressBc7(original, Bc7Options{}, testJobs());
        assert(compressed.width == 64 && compressed.height == 48);
        assert(compressed.blocks.size() == bc7LevelBytes(64, 48));

        const ImageLevel decoded = decompressBc7(compressed);
        assert(decoded.width == 64 && decoded.height == 48 && decoded.pixels.size() == original.pixels.size());
        assert(colourPsnr(original, decoded) > 41.5);

        // Solid alpha may come back as 254: bc7enc's mode 6 shares an endpoint's last bit between colour and alpha and
        // may give alpha's up for a closer colour. Harmless: cut-outs test against a cutoff, opaque surfaces ignore alpha.
        for (std::size_t alpha = 3; alpha < decoded.pixels.size(); alpha += 4) {
            assert(decoded.pixels[alpha] >= 254);
        }

        const ImageLevel wave = gradientImage(64, 48);
        assert(colourPsnr(wave, decompressBc7(compressBc7(wave, Bc7Options{.perceptual = false}, testJobs()))) > 39.5);
        assert(colourPsnr(wave, decompressBc7(compressBc7(wave, Bc7Options{}, testJobs()))) > 37.5);
    }

    // Sizes that are not multiples of four: the edge blocks are filled out by repeating the last row and column, and
    // the decoded picture has exactly the original's texels, close to them. A gentle slope (6 and 5 levels a texel, as
    // a real texture's shading), since a block spanning most of the range in two directions is beyond any BC7 encoder's
    // few levels (a full-range ramp over 6 texels misses by 29).
    void testOddSizes() {
        for (const auto& [width, height] : {std::pair{6u, 5u}, std::pair{1u, 1u}, std::pair{2u, 7u}, std::pair{13u, 3u}}) {
            ImageLevel original{width, height, {}};

            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    original.pixels.insert(original.pixels.end(), {static_cast<std::uint8_t>(40 + 6 * x), static_cast<std::uint8_t>(30 + 5 * y), 60, 255});
                }
            }

            const ImageLevel decoded = decompressBc7(compressBc7(original, Bc7Options{}, testJobs()));
            assert(decoded.width == width && decoded.height == height && decoded.pixels.size() == original.pixels.size());

            for (std::size_t index = 0; index < original.pixels.size(); ++index) {
                assert(std::abs(int{original.pixels[index]} - int{decoded.pixels[index]}) <= 8);
            }
        }
    }

    // A cut-out's holes survive: texels solid or clear stay on the same side of the cutoff, so no leaf grows or
    // loses texels to compression.
    void testCutOutAlphaKeepsItsSide() {
        ImageLevel original = gradientImage(32, 32);

        for (std::uint32_t texel = 0; texel < 32 * 32; ++texel) {
            const auto bits = static_cast<std::uint32_t>(mixBits(texel));
            original.pixels[texel * 4 + 3] = bits % 100 < 40 ? 255 : static_cast<std::uint8_t>(bits % 40);
        }

        const ImageLevel decoded = decompressBc7(compressBc7(original, Bc7Options{}, testJobs()));
        const auto cutoff = static_cast<int>(0.5f * 255.0f);

        for (std::size_t alpha = 3; alpha < original.pixels.size(); alpha += 4) {
            assert((original.pixels[alpha] >= cutoff) == (decoded.pixels[alpha] >= cutoff));
        }
    }

    // Normal maps (x, y, z stored as n * 0.5 + 0.5) compressed as plain data come back pointing within a few degrees,
    // and their length (which Toksvig reads) within a few percent. Measured on these bumps (about 7 degrees a texel at
    // their steepest): 1.33 degrees off on average, 6.0 at worst, length within 0.034 (quality 4: 1.21, 5.7, 0.024).
    void testNormalsStayClose() {
        ImageLevel original{32, 32, {}};

        for (std::uint32_t y = 0; y < 32; ++y) {
            for (std::uint32_t x = 0; x < 32; ++x) {
                const double nx = 0.4 * std::sin(0.3 * x);
                const double ny = 0.3 * std::cos(0.2 * y);
                const double nz = std::sqrt(1.0 - nx * nx - ny * ny);
                const auto store = [](double component) { return static_cast<std::uint8_t>(std::lround((component * 0.5 + 0.5) * 255.0)); };
                original.pixels.insert(original.pixels.end(), {store(nx), store(ny), store(nz), 255});
            }
        }

        const ImageLevel decoded = decompressBc7(compressBc7(original, Bc7Options{.perceptual = false}, testJobs()));
        const auto read = [](const ImageLevel& image, std::size_t texel, int channel) { return image.pixels[texel * 4 + channel] / 255.0 * 2.0 - 1.0; };

        double meanAngle = 0.0;

        for (std::size_t texel = 0; texel < 32 * 32; ++texel) {
            double dot = 0.0;
            double decodedLength = 0.0;
            double originalLength = 0.0;

            for (int channel = 0; channel < 3; ++channel) {
                dot += read(original, texel, channel) * read(decoded, texel, channel);
                decodedLength += read(decoded, texel, channel) * read(decoded, texel, channel);
                originalLength += read(original, texel, channel) * read(original, texel, channel);
            }

            decodedLength = std::sqrt(decodedLength);
            originalLength = std::sqrt(originalLength);
            const double angle = std::acos(std::clamp(dot / (decodedLength * originalLength), -1.0, 1.0));
            assert(angle < 7.0 * 3.14159265358979 / 180.0);
            assert(std::abs(decodedLength - originalLength) < 0.04);
            meanAngle += angle / (32.0 * 32.0);
        }

        assert(meanAngle < 1.6 * 3.14159265358979 / 180.0);
    }

    // The blocks are the same whichever threads encode them, so the texture cache's files do not depend on the machine's cores.
    void testSameBlocksOnAnyThreads() {
        const ImageLevel original = gradientImage(40, 24);
        const CompressedLevel threaded = compressBc7(original, Bc7Options{}, testJobs());
        const CompressedLevel serial = compressBc7(original, Bc7Options{}, serialJobs());
        assert(threaded.blocks == serial.blocks);
    }

    // Every level below the full size, each the one above halved as halveImage does, down to one texel.
    void testAveragedMips() {
        const ImageLevel full = gradientImage(16, 4);
        const std::vector<ImageLevel> levels = averagedMips(full, true);
        assert(levels.size() == 4);
        assert(levels[0].width == 8 && levels[0].height == 2);
        assert(levels[3].width == 1 && levels[3].height == 1);
        assert(levels[0].pixels == halveImage(full, true).pixels);
        assert(levels[1].pixels == halveImage(levels[0], true).pixels);
        assert(averagedMips(gradientImage(1, 1), false).empty());
    }
}

void testBlockCompression() {
    testLevelSizes();
    testRoundTripKeepsAGradient();
    testOddSizes();
    testCutOutAlphaKeepsItsSide();
    testNormalsStayClose();
    testSameBlocksOnAnyThreads();
    testAveragedMips();
}
