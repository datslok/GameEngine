#ifndef MATH_CLOSEST_POINTS_SHARED_H
#define MATH_CLOSEST_POINTS_SHARED_H

// Shared by closest_points.cpp and the shaders that mirror it (cloth.glsl). Only #defines of plain numbers, since GLSL
// includes this file too.

// Below this squared length a segment is treated as a single point, so nothing divides by (nearly) zero.
#define CLOSEST_POINTS_DEGENERATE_LENGTH_SQUARED 1e-12

#endif
