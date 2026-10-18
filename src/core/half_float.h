#pragma once

#include <cstdint>

/*
* Half floats (IEEE 754 binary16): 1 sign bit, 5 exponent bits (bias 15), 10 fraction bits. GPUs store 16-bit float
* images this way (the HDR scene image, the scRGB screen image), and the golden image check reads them back.
*/

// The value of a half float's bits, exactly (every half is a float).
float halfToFloat(std::uint16_t bits);

// What halfStepDistance returns when one side is NaN and the bits differ: as far apart as anything can be.
inline constexpr std::uint32_t halfStepDistanceNever = 0xFFFFFFFFu;

/*
* How many representable halves apart two values are (units in the last place). Neighbours are 1 apart at any size, so
* one tolerance means the same for dark and bright pixels; +0 and -0 are 0 apart.
*/
std::uint32_t halfStepDistance(std::uint16_t first, std::uint16_t second);
