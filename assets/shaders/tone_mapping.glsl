// The tone mapping curves, included with #include "tone_mapping.glsl": mirrors render/gpu/tone_mapping.cpp, which tests
// the maths, and tests/gpu checks these functions against it on the GPU.

#ifndef TONE_MAPPING_GLSL
#define TONE_MAPPING_GLSL

// The curves' numbers, shared with render/gpu/tone_mapping.cpp.
#include "render/gpu/tone_mapping_shared.h"

vec3 reinhard(vec3 value) {
    return value / (1.0 + value);
}

vec3 aces(vec3 value) {
    return (value * (ACES_A * value + ACES_B)) / (value * (ACES_C * value + ACES_D) + ACES_E);
}

// The S-curve over the stops range, t in 0..1, by Horner's rule.
vec3 agxContrast(vec3 t) {
    vec3 result = vec3(AGX_C6);
    result = result * t + AGX_C5;
    result = result * t + AGX_C4;
    result = result * t + AGX_C3;
    result = result * t + AGX_C2;
    result = result * t + AGX_C1;
    return result * t + AGX_C0;
}

// AgX: mix the channels slightly, curve them in stops (a log scale), unmix and undo the display gamma.
// GLSL builds matrices column by column, so the row-by-row numbers are listed down each column.
vec3 agx(vec3 value) {
    const mat3 inset = mat3(
        AGX_INSET_00, AGX_INSET_10, AGX_INSET_20,
        AGX_INSET_01, AGX_INSET_11, AGX_INSET_21,
        AGX_INSET_02, AGX_INSET_12, AGX_INSET_22);
    const mat3 outset = mat3(
        AGX_OUTSET_00, AGX_OUTSET_10, AGX_OUTSET_20,
        AGX_OUTSET_01, AGX_OUTSET_11, AGX_OUTSET_21,
        AGX_OUTSET_02, AGX_OUTSET_12, AGX_OUTSET_22);

    vec3 stops = clamp(log2(max(inset * value, vec3(1e-10))), AGX_MIN_STOPS, AGX_MAX_STOPS);
    vec3 encoded = outset * agxContrast((stops - AGX_MIN_STOPS) / (AGX_MAX_STOPS - AGX_MIN_STOPS));
    return pow(clamp(encoded, 0.0, 1.0), vec3(AGX_DISPLAY_GAMMA));
}

// The curve for a screen whose peak is 1.
vec3 applyCurve(int curve, vec3 value) {
    if (curve == TONE_MAP_REINHARD) {
        return reinhard(value);
    }

    if (curve == TONE_MAP_ACES) {
        return aces(value);
    }

    if (curve == TONE_MAP_AGX) {
        return agx(value);
    }

    return min(value, vec3(1.0));
}

/*
* Light (zero or more) scaled by the exposure, measured in units of the screen's peak, curved into 0..1 and scaled back:
* toneMap in tone_mapping.cpp, which also clamps negative light to 0 (the shader does that before mixing in bloom).
*/
vec3 toneMap(vec3 linear, float exposure, float peak, int curve) {
    return clamp(applyCurve(curve, linear * (exposure / peak)), 0.0, 1.0) * peak;
}

#endif
