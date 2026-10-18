#include "core/half_float.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace {
    constexpr std::uint16_t signBit = 0x8000;
    constexpr std::uint16_t magnitudeBits = 0x7FFF;
    constexpr std::uint16_t infinityBits = 0x7C00;

    bool isNan(std::uint16_t bits) {
        return (bits & magnitudeBits) > infinityBits;
    }

    /*
    * Halves of one sign are ordered like their bits read as integers, so mapping negatives below zero (sign and
    * magnitude to a signed integer) puts every half on one number line where neighbours differ by 1.
    */
    std::int32_t orderedSteps(std::uint16_t bits) {
        const std::int32_t magnitude = bits & magnitudeBits;
        return (bits & signBit) != 0 ? -magnitude : magnitude;
    }
}

/*
* value = (-1)^sign * 2^(exponent - 15) * 1.fraction for normal numbers; exponent 0 holds the subnormals,
* 2^-14 * 0.fraction, and exponent 31 infinity (fraction 0) or NaN.
*/
float halfToFloat(std::uint16_t bits) {
    const float sign = (bits & signBit) != 0 ? -1.0f : 1.0f;
    const int exponent = (bits >> 10) & 0x1F;
    const int fraction = bits & 0x3FF;

    if (exponent == 0) {
        return sign * std::ldexp(static_cast<float>(fraction), -24);
    }

    if (exponent == 31) {
        return fraction == 0 ? sign * std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    }

    return sign * std::ldexp(static_cast<float>(fraction + 1024), exponent - 25);
}

std::uint32_t halfStepDistance(std::uint16_t first, std::uint16_t second) {
    if (first == second) {
        return 0;
    }

    if (isNan(first) || isNan(second)) {
        return halfStepDistanceNever;
    }

    const std::int32_t difference = orderedSteps(first) - orderedSteps(second);
    return static_cast<std::uint32_t>(difference < 0 ? -difference : difference);
}
