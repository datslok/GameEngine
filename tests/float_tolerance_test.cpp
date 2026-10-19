#include "core/float_tolerance.h"

#include <cassert>
#include <cmath>
#include <limits>

namespace {
    /*
    * The allowed difference is absolute + relative * |expected|: the absolute part covers values near zero, where any
    * relative test fails (0.0 and 1e-30 differ by "100%"), the relative part covers large values, where a fixed
    * difference is far below one float step (at a million a step is 0.0625).
    */
    void testAllowanceMixesAbsoluteAndRelative() {
        const FloatTolerance tolerance{.absolute = 1e-6f, .relative = 1e-4f};

        // At 1 the allowance is 1e-6 + 1e-4, so 1e-4 off uses most of it.
        assert(withinTolerance(1.0f, 1.0001f, tolerance));
        assert(!withinTolerance(1.0f, 1.0002f, tolerance));

        // At 1000 it is about 0.1.
        assert(withinTolerance(1000.0f, 1000.09f, tolerance));
        assert(!withinTolerance(1000.0f, 1000.2f, tolerance));

        // Near zero only the absolute part is left.
        assert(withinTolerance(0.0f, 9e-7f, tolerance));
        assert(!withinTolerance(0.0f, 2e-6f, tolerance));

        // Symmetric in the sign of the difference.
        assert(withinTolerance(1.0f, 0.9999f, tolerance));
    }

    // The share of the allowance used, so a report can show how much room is left: 0 when equal, 1 at the edge.
    void testUseMeasuresTheMargin() {
        const FloatTolerance tolerance{.absolute = 1.0f, .relative = 0.0f};
        assert(toleranceUsed(5.0f, 5.0f, tolerance) == 0.0f);
        assert(std::abs(toleranceUsed(5.0f, 5.5f, tolerance) - 0.5f) < 1e-6f);
        assert(std::abs(toleranceUsed(5.0f, 3.0f, tolerance) - 2.0f) < 1e-6f);
    }

    /*
    * Special values must never pass by accident: a NaN compares false with everything, so a careless |a - b| <= t would
    * reject it, but a careless !(|a - b| > t) would accept it. Infinities pass only when both sides are the same infinity.
    */
    void testSpecialValues() {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();
        const FloatTolerance tolerance{.absolute = 1.0f, .relative = 1.0f};

        assert(!withinTolerance(1.0f, nan, tolerance));
        assert(!withinTolerance(nan, 1.0f, tolerance));
        assert(!withinTolerance(nan, nan, tolerance));
        assert(std::isinf(toleranceUsed(1.0f, nan, tolerance)));

        assert(withinTolerance(infinity, infinity, tolerance));
        assert(!withinTolerance(infinity, -infinity, tolerance));
        assert(!withinTolerance(infinity, 1e30f, tolerance));
    }

    // A zero tolerance asks for exact equality (integer results such as a cube face number).
    void testZeroToleranceIsExact() {
        const FloatTolerance exact{};
        assert(withinTolerance(3.0f, 3.0f, exact));
        assert(!withinTolerance(3.0f, std::nextafter(3.0f, 4.0f), exact));
        assert(toleranceUsed(0.0f, 0.0f, exact) == 0.0f);
    }

    // The tally keeps the worst comparison of a series, for the report, and counts the failures.
    void testTallyKeepsTheWorstCase() {
        ToleranceTally tally{FloatTolerance{.absolute = 1.0f, .relative = 0.0f}};
        assert(tally.passes());
        assert(tally.checked == 0);

        tally.add(10.0f, 10.25f);
        tally.add(20.0f, 20.5f);
        tally.add(30.0f, 30.0f);
        assert(tally.passes());
        assert(tally.checked == 3);
        assert(std::abs(tally.worstUse - 0.5f) < 1e-6f);
        assert(tally.worstExpected == 20.0f);
        assert(tally.worstActual == 20.5f);

        tally.add(40.0f, 42.0f);
        assert(!tally.passes());
        assert(tally.failures == 1);
        assert(tally.worstExpected == 40.0f);
    }

    // A comparison can bring its own tolerance, for results whose sensitivity changes from case to case.
    void testTallyTakesATolerancePerComparison() {
        ToleranceTally tally{FloatTolerance{.absolute = 1.0f, .relative = 0.0f}};
        tally.add(10.0f, 14.0f, FloatTolerance{.absolute = 8.0f, .relative = 0.0f});
        assert(tally.passes());
        assert(std::abs(tally.worstUse - 0.5f) < 1e-6f);

        tally.add(10.0f, 14.0f);
        assert(!tally.passes());
    }
}

void testFloatTolerance() {
    testAllowanceMixesAbsoluteAndRelative();
    testUseMeasuresTheMargin();
    testSpecialValues();
    testZeroToleranceIsExact();
    testTallyKeepsTheWorstCase();
    testTallyTakesATolerancePerComparison();
}
