#include "core/srgb.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0005f;
    }

    // Black and white are the same in both encodings; only the values in between are curved.
    void testEndpointsAreUnchanged() {
        assert(srgbToLinear(0.0f) == 0.0f);
        assert(nearlyEqual(srgbToLinear(1.0f), 1.0f));
        assert(linearToSrgb(0.0f) == 0.0f);
        assert(nearlyEqual(linearToSrgb(1.0f), 1.0f));
    }

    // A stored mid-grey (128 of 255) is only about a fifth of white's light: the curve spends most values on dark tones.
    void testMidGreyIsDarkInLinearLight() {
        assert(nearlyEqual(srgbByteToLinear(128), 0.2158f));
        assert(nearlyEqual(linearToSrgb(0.5f), 0.7354f));
    }

    // Near black the curve is a straight line (divide by 12.92), so tiny values do not blow up.
    void testDarkValuesUseTheLinearSegment() {
        assert(nearlyEqual(srgbToLinear(0.04f), 0.04f / 12.92f));
        assert(nearlyEqual(linearToSrgb(0.003f), 0.003f * 12.92f));
    }

    // Decoding then encoding gives back every byte, so colours picked as bytes survive the trip through linear light.
    void testBytesRoundTrip() {
        for (int value = 0; value <= 255; ++value) {
            const std::uint8_t byte = static_cast<std::uint8_t>(value);
            assert(linearToSrgbByte(srgbByteToLinear(byte)) == byte);
        }
    }
}

void testSrgb() {
    testEndpointsAreUnchanged();
    testMidGreyIsDarkInLinearLight();
    testDarkValuesUseTheLinearSegment();
    testBytesRoundTrip();
}
