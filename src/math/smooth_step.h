#pragma once

#include <algorithm>

/*
* 0 below edge0, 1 above edge1, and the smooth 3t^2 - 2t^3 between (t clamped to 0..1), as GLSL's smoothstep defines
* it, so the C++ mirrors of shader maths match the GPU. edge1 must be above edge0.
*/
inline float smoothStep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
