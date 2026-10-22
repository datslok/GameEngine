#include "render/gpu/light_falloff.h"
#include "math/smooth_step.h"

#include <algorithm>

/*
* Written in the shader's order of operations, so the GPU tests compare like with like: the ratio's fourth power as
* three multiplications, the window squared, then one division.
*/
float distanceFalloff(float distance, float range, float sourceRadius) {
    const float ratio = distance / range;
    const float window = std::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
    return window * window / (distance * distance + sourceRadius * sourceRadius);
}

float spotCone(float alongBeam, float cosOuter, float cosInner) {
    return smoothStep(cosOuter, cosInner, alongBeam);
}
