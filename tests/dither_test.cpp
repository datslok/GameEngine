#include "core/dither.h"
#include "core/srgb.h"

#include <cassert>
#include <cmath>
#include <initializer_list>

namespace {
    // What the screen does with the shader's output: encode to sRGB and round to one of 256 levels.
    float storedLevel(float linear) {
        return std::round(std::fmin(std::fmax(linearToSrgb(linear), 0.0f), 1.0f) * 255.0f);
    }

    // The per-pixel noise stays in 0..1 and is spread evenly: a 64 by 64 patch averages to a half, with each tenth
    // of the range about equally common.
    void testNoiseIsEvenlySpread() {
        int buckets[10] = {};
        double sum = 0.0;

        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const float noise = interleavedGradientNoise(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                assert(noise >= 0.0f && noise < 1.0f);
                sum += noise;
                ++buckets[static_cast<int>(noise * 10.0f)];
            }
        }

        assert(std::abs(sum / 4096.0 - 0.5) < 0.02);

        for (int count : buckets) {
            assert(count > 330 && count < 490);
        }
    }

    // Averaged over many pixels, dithered and rounded values give back the true brightness, so dithering removes the
    // steps without making anything brighter or darker. Checked on dark values, where banding shows most.
    void testAverageKeepsTheTrueBrightness() {
        for (float linear : {0.0007f, 0.0031f, 0.0123f, 0.05f, 0.2f}) {
            double sum = 0.0;

            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
                    const float noise = interleavedGradientNoise(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                    sum += storedLevel(ditherForEightBits(linear, noise));
                }
            }

            const double expected = linearToSrgb(linear) * 255.0;
            assert(std::abs(sum / 4096.0 - expected) < 0.05);
        }
    }

    // A value exactly on one of the 256 levels never moves, so flat colours do not turn grainy.
    void testExactLevelsStayExact() {
        for (int level : {0, 1, 17, 128, 255}) {
            const float linear = srgbToLinear(static_cast<float>(level) / 255.0f);

            for (float noise : {0.0f, 0.25f, 0.5f, 0.999f}) {
                assert(storedLevel(ditherForEightBits(linear, noise)) == static_cast<float>(level));
            }
        }
    }
}

void testDither() {
    testNoiseIsEvenlySpread();
    testAverageKeepsTheTrueBrightness();
    testExactLevelsStayExact();
}