#pragma once

#include <cstddef>

/*
* How close two floats must be to count as the same result, for comparing one computation done two ways (a shader on
* the GPU against its C++ mirror). Exact equality is the wrong test there: the GPU fuses multiply-adds into one
* rounding, and Vulkan lets pow, exp2, log2, inversesqrt and division be a few units in the last place off, so the same
* formula lands a float step or a few away. The allowed difference is absolute + relative * |expected|: the absolute
* part covers results near zero, where a relative test fails, the relative part covers large ones, where a fixed
* difference would be smaller than one float step. All zero asks for exact equality.
*/
struct FloatTolerance {
    float absolute = 0.0f;
    float relative = 0.0f;
};

/*
* The share of the allowed difference that actual uses: 0 when equal, 1 at the edge, above 1 failing. Infinite when
* either value is a NaN, or the values differ and nothing is allowed; two equal infinities count as equal.
*/
float toleranceUsed(float expected, float actual, const FloatTolerance& tolerance);

bool withinTolerance(float expected, float actual, const FloatTolerance& tolerance);

/*
* A series of comparisons under one tolerance: how many were checked and failed, and the worst one (the largest share
* used), so a report can show how much room the tolerance leaves as well as whether it passed.
*/
struct ToleranceTally {
    FloatTolerance tolerance;
    std::size_t checked = 0;
    std::size_t failures = 0;
    float worstUse = 0.0f;
    float worstExpected = 0.0f;
    float worstActual = 0.0f;

    void add(float expected, float actual);

    // With a tolerance of its own instead of the tally's, for results whose sensitivity changes from case to case.
    void add(float expected, float actual, const FloatTolerance& caseTolerance);

    bool passes() const;
};
