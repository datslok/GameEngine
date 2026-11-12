// Contact shadows (render/gpu/contact_shadows.h): the march's steps, the test of a step against the depth image and
// the fades. Pure functions, mirrored by render/gpu/contact_shadows.cpp, which explains each, and checked against it by
// the GPU tests; the march itself is contact_shadows.comp's.

#ifndef CONTACT_SHADOWS_GLSL
#define CONTACT_SHADOWS_GLSL

#include "render/gpu/contact_shadows_shared.h"

float contactStepDistance(int stepIndex, int steps, float noise, float marchLength) {
    return (float(stepIndex) + noise) / float(steps) * marchLength;
}

float contactBias(float footprint, float viewDistance) {
    return float(CONTACT_BIAS_PIXELS) * footprint * viewDistance;
}

bool contactBlocked(float rayDistance, float sceneDistance, float bias, float thickness) {
    float behind = rayDistance - sceneDistance;
    return behind > bias && behind < thickness;
}

float contactEdgeFade(vec2 pixel, vec2 size) {
    float nearest = min(min(pixel.x, pixel.y), min(size.x - pixel.x, size.y - pixel.y));
    return clamp(nearest / float(CONTACT_EDGE_PIXELS), 0.0, 1.0);
}

float contactDistanceFade(float viewDistance, float fadeStart, float fadeEnd) {
    if (!(fadeEnd > fadeStart)) {
        return viewDistance <= fadeStart ? 1.0 : 0.0;
    }

    return clamp((fadeEnd - viewDistance) / (fadeEnd - fadeStart), 0.0, 1.0);
}

#endif
