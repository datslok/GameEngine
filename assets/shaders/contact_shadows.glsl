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

float contactStepReach(float marchLength, int steps) {
    return marchLength / float(steps);
}

bool contactBlocked(float previousBehind, float behind, float bias, float reach) {
    return previousBehind <= bias && behind > bias && behind < bias + reach;
}

float contactLightStop(float emitterRadius) {
    return max(emitterRadius, 0.0) + float(CONTACT_LIGHT_CLEARANCE);
}

// contactLightRay: xyz the unit way to the light, w the march's length; w 0 for none.
vec4 contactLightRay(vec4 positionRange, vec4 beamCosOuter, float stopDistance, vec3 start, float maxLength) {
    vec3 offset = positionRange.xyz - start;
    float distance = length(offset);

    if (!(distance < positionRange.w) || !(distance > stopDistance)) {
        return vec4(0.0);
    }

    vec3 toLight = offset * (1.0 / distance);

    if (!(dot(toLight, beamCosOuter.xyz) * -1.0 > beamCosOuter.w)) {
        return vec4(0.0);
    }

    return vec4(toLight, min(maxLength, distance - stopDistance));
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
