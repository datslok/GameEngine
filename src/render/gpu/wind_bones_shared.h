#ifndef WIND_BONES_SHARED_H
#define WIND_BONES_SHARED_H

// Trees' bones' motion (render/gpu/wind_bones.h, wind_bones.glsl), numbers shared by C++ and GLSL.

// A cantilever's first mode: f = WIND_BONE_FIRST_MODE sqrt(E I / (rho A)) / L^2, 1.8751^2 / (2 pi); for a round
// section sqrt(I / A) is r / 2.
#define WIND_BONE_FIRST_MODE 0.559597

// A bone's radius is floored at this (metres) before its stiffness is taken, so a zero radius never divides by zero.
#define WIND_BONE_THINNEST 0.001

#endif
