#pragma once

#include <numbers>

// One full turn, 2 pi radians.
inline constexpr float fullTurnRadians = 2.0f * std::numbers::pi_v<float>;

// Degrees to radians, for values people think about in degrees (fields of view, pitch limits, crease angles).
constexpr float radiansFromDegrees(float degrees) {
    return degrees * std::numbers::pi_v<float> / 180.0f;
}

// Radians to degrees, for showing angles to people (the slope in the debug readout).
constexpr float degreesFromRadians(float radians) {
    return radians * 180.0f / std::numbers::pi_v<float>;
}

// The same direction as an angle in -pi..pi. Applied to a difference of two angles, it gives the shortest turn between them.
float wrapAngle(float radians);
