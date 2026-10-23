#include "render/gpu/light_falloff.h"
#include "math/smooth_step.h"

#include <algorithm>

/*
* Written in the shader's order of operations, so the GPU tests compare like with like: the ratio's fourth power as
* three multiplications, the window squared, then one division.
*/
float rangeWindow(float distance, float range) {
    const float ratio = distance / range;
    return std::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
}

float distanceFalloff(float distance, float range, float sourceRadius) {
    const float window = rangeWindow(distance, range);
    return window * window / (distance * distance + sourceRadius * sourceRadius);
}

float spotCone(float alongBeam, float cosOuter, float cosInner) {
    return smoothStep(cosOuter, cosInner, alongBeam);
}
