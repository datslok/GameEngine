#include "core/float_tolerance.h"

#include <cmath>
#include <limits>

/*
* Equal values (two equal infinities included) use nothing. Otherwise a NaN anywhere, or a difference that is itself
* infinite or not allowed at all, uses an infinite share, so it can never pass. The comparison is written so a NaN
* falls into the failing branch rather than slipping through a comparison that is false.
*/
float toleranceUsed(float expected, float actual, const FloatTolerance& tolerance) {
    if (expected == actual) {
        return 0.0f;
    }

    const float infinity = std::numeric_limits<float>::infinity();
    const float difference = std::abs(actual - expected);
    const float allowed = tolerance.absolute + tolerance.relative * std::abs(expected);

    if (!std::isfinite(difference) || !(allowed > 0.0f)) {
        return infinity;
    }

    return difference / allowed;
}

bool withinTolerance(float expected, float actual, const FloatTolerance& tolerance) {
    return toleranceUsed(expected, actual, tolerance) <= 1.0f;
}

void ToleranceTally::add(float expected, float actual) {
    add(expected, actual, tolerance);
}

void ToleranceTally::add(float expected, float actual, const FloatTolerance& caseTolerance) {
    const float used = toleranceUsed(expected, actual, caseTolerance);
    ++checked;

    if (!(used <= 1.0f)) {
        ++failures;
    }

    if (checked == 1 || used > worstUse) {
        worstUse = used;
        worstExpected = expected;
        worstActual = actual;
    }
}

bool ToleranceTally::passes() const {
    return failures == 0;
}
