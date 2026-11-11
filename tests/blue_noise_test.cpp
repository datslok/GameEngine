#include "core/blue_noise.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
    constexpr int size = 32;

    // Every rank once: the values sorted are exactly (k + 0.5) / texels.
    void testValuesEven() {
        std::vector<float> values = makeBlueNoise(size, 7u);
        assert(values.size() == static_cast<std::size_t>(size * size));
        std::sort(values.begin(), values.end());

        for (std::size_t k = 0; k < values.size(); ++k) {
            assert(values[k] == (static_cast<float>(k) + 0.5f) / static_cast<float>(values.size()));
        }
    }

    // The same seed gives the same tile; another seed another.
    void testDeterministic() {
        assert(makeBlueNoise(size, 3u) == makeBlueNoise(size, 3u));
        assert(makeBlueNoise(size, 3u) != makeBlueNoise(size, 4u));
    }

    /*
    * Blue: little energy at low frequencies, so the means of 4 x 4 blocks barely vary (white noise's would vary by the
    * values' variance, 1/12, over 16), and next-door texels differ more than white noise's do (a third on average).
    */
    void testBlue() {
        const std::vector<float> values = makeBlueNoise(size, 11u);
        double sum = 0.0;
        double squares = 0.0;
        int blocks = 0;

        for (int by = 0; by < size; by += 4) {
            for (int bx = 0; bx < size; bx += 4) {
                double mean = 0.0;

                for (int y = by; y < by + 4; ++y) {
                    for (int x = bx; x < bx + 4; ++x) {
                        mean += values[static_cast<std::size_t>(y * size + x)] / 16.0;
                    }
                }

                sum += mean;
                squares += mean * mean;
                ++blocks;
            }
        }

        const double meanOfMeans = sum / blocks;
        const double blockVariance = squares / blocks - meanOfMeans * meanOfMeans;
        assert(blockVariance < 0.2 * (1.0 / 12.0) / 16.0);

        double neighbours = 0.0;

        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float here = values[static_cast<std::size_t>(y * size + x)];
                neighbours += std::abs(here - values[static_cast<std::size_t>(y * size + (x + 1) % size)]);
                neighbours += std::abs(here - values[static_cast<std::size_t>(((y + 1) % size) * size + x)]);
            }
        }

        assert(neighbours / (2.0 * size * size) > 0.38);
    }

    // Tiles from two seeds are unrelated: their correlation is near zero.
    void testSeedsIndependent() {
        const std::vector<float> a = makeBlueNoise(size, 1u);
        const std::vector<float> b = makeBlueNoise(size, 2u);
        double product = 0.0;

        for (std::size_t i = 0; i < a.size(); ++i) {
            product += (a[i] - 0.5) * (b[i] - 0.5);
        }

        const double correlation = product / static_cast<double>(a.size()) / (1.0 / 12.0);
        assert(std::abs(correlation) < 0.1);
    }
}

void testBlueNoise() {
    testValuesEven();
    testDeterministic();
    testBlue();
    testSeedsIndependent();
}