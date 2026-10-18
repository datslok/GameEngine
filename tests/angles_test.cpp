#include "math/angles.h"

#include <cassert>
#include <cmath>
#include <numbers>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }
}

void testAngles() {
    constexpr float pi = std::numbers::pi_v<float>;

    // Degrees and radians, both ways, and back again.
    assert(nearlyEqual(radiansFromDegrees(180.0f), pi));
    assert(nearlyEqual(radiansFromDegrees(-90.0f), -pi / 2.0f));
    assert(nearlyEqual(degreesFromRadians(pi / 4.0f), 45.0f));
    assert(nearlyEqual(degreesFromRadians(radiansFromDegrees(37.0f)), 37.0f));
    assert(nearlyEqual(fullTurnRadians, radiansFromDegrees(360.0f)));

    // Wrapping into -pi..pi keeps the direction: a turn and a half is half a turn.
    assert(nearlyEqual(wrapAngle(3.0f * pi / 2.0f), -pi / 2.0f));
    assert(nearlyEqual(wrapAngle(-3.0f * pi / 2.0f), pi / 2.0f));
    assert(nearlyEqual(wrapAngle(0.25f), 0.25f));
}
