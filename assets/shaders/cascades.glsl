// Choosing between a sun's shadow cascades, included with #include "cascades.glsl": mirrors render/gpu/cascades.cpp,
// which tests it, and tests/gpu checks these functions against it on the GPU.

#ifndef CASCADES_GLSL
#define CASCADES_GLSL

#include "render/gpu/shader_shared.h"

const float maxSunBiasSlope = MAX_SUN_BIAS_SLOPE;

// How far a point is inside a cascade's box in units of its radius: 1 at the centre, 0 on an edge, negative outside.
float cascadeEdgeDistance(vec3 projected, float depthRangeOverRadius) {
    float across = 1.0 - max(abs(projected.x), abs(projected.y));
    float fromFarSide = projected.z * depthRangeOverRadius;
    float fromNearSide = (1.0 - projected.z) * depthRangeOverRadius;
    return min(across, min(fromFarSide, fromNearSide));
}

// This cascade's share of the shadow: 1 deeper than the band inside its box, falling to 0 at the edge.
float cascadeBlendWeight(float edgeDistance, float blendBand) {
    if (blendBand <= 0.0) {
        return edgeDistance > 0.0 ? 1.0 : 0.0;
    }

    return clamp(edgeDistance / blendBand, 0.0, 1.0);
}

// The dithered switch: whether this pixel takes its shadow from this cascade alone.
bool cascadeDitherKeeps(float weight, float noise) {
    return noise < weight;
}

// How far to move a point towards the sun before its shadow lookup: the base offset, more on slopes the sun grazes.
float sunShadowBias(float baseOffset, float nDotL) {
    float facing = clamp(nDotL, 0.0, 1.0);
    float slope = sqrt(1.0 - facing * facing) / max(facing, 1e-6);
    return baseOffset * (1.0 + min(slope, maxSunBiasSlope));
}

#endif
