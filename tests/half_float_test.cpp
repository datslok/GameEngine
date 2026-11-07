#include "core/half_float.h"

#include <cassert>
#include <cmath>
#include <cstdint>

namespace {
    // Half floats: 1 sign bit, 5 exponent bits (bias 15) and 10 fraction bits, as GPUs store 16-bit float images.
    void testKnownValues() {
        assert(halfToFloat(0x0000) == 0.0f);
        assert(halfToFloat(0x3C00) == 1.0f);
        assert(halfToFloat(0xC000) == -2.0f);
        assert(halfToFloat(0x3800) == 0.5f);
        assert(halfToFloat(0x7BFF) == 65504.0f);            // the largest finite half
        assert(halfToFloat(0x3555) == 0.333251953125f);     // the nearest half to a third
        assert(halfToFloat(0x0001) == std::ldexp(1.0f, -24)); // the smallest subnormal
        assert(halfToFloat(0x0400) == std::ldexp(1.0f, -14)); // the smallest normal
        assert(std::isinf(halfToFloat(0x7C00)) && halfToFloat(0x7C00) > 0.0f);
        assert(std::isnan(halfToFloat(0x7E00)));
        assert(std::signbit(halfToFloat(0x8000)));
    }

    /*
    * The distance in representable steps (units in the last place). Neighbouring halves are 1 apart whatever their size,
    * which makes a tolerance that means the same thing for dark and bright pixels; +0 and -0 are the same number.
    */
    void testStepDistance() {
        assert(halfStepDistance(0x3C00, 0x3C00) == 0);
        assert(halfStepDistance(0x3C00, 0x3C01) == 1);
        assert(halfStepDistance(0x3C01, 0x3C00) == 1);
        assert(halfStepDistance(0x0000, 0x8000) == 0);
        assert(halfStepDistance(0x0001, 0x8001) == 2);
        assert(halfStepDistance(0x3BFF, 0x3C00) == 1); // across an exponent boundary
        assert(halfStepDistance(0x3C00, 0xBC00) == 2 * 0x3C00);
    }

    // A NaN equals only the very same bits; against anything else it is as far as possible.
    void testNanIsNeverClose() {
        assert(halfStepDistance(0x7E00, 0x7E00) == 0);
        assert(halfStepDistance(0x7E00, 0x3C00) == halfStepDistanceNever);
        assert(halfStepDistance(0x3C00, 0x7E01) == halfStepDistanceNever);
    }

    // Back from a float: every half comes back as itself, ties go to the even half, and what a half cannot hold saturates.
    void testFloatToHalf() {
        for (std::uint32_t bits = 0; bits <= 0xFFFFu; ++bits) {
            const std::uint16_t half = static_cast<std::uint16_t>(bits);
            if ((half & 0x7FFFu) <= 0x7C00u) {
                assert(floatToHalf(halfToFloat(half)) == half);
            }
        }

        assert(floatToHalf(1.0f + std::ldexp(1.0f, -11)) == 0x3C00);       // halfway from 1 up: to 1, the even one
        assert(floatToHalf(1.0f + 3.0f * std::ldexp(1.0f, -11)) == 0x3C02); // halfway from the odd 0x3C01 up: to 0x3C02
        assert(floatToHalf(1.0f + std::ldexp(1.0f, -12)) == 0x3C00);       // under halfway: down
        assert(floatToHalf(1.0f / 3.0f) == 0x3555);
        assert(floatToHalf(65519.0f) == 0x7BFF && floatToHalf(65520.0f) == 0x7C00 && floatToHalf(-1e9f) == 0xFC00);
        assert(floatToHalf(std::ldexp(1.0f, -26)) == 0x0000 && floatToHalf(std::ldexp(3.0f, -25)) == 0x0002);
        assert((floatToHalf(std::nanf("")) & 0x7FFFu) > 0x7C00u);
    }
}

void testHalfFloat() {
    testKnownValues();
    testStepDistance();
    testNanIsNeverClose();
    testFloatToHalf();
}
