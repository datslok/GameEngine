// Depth of field (render/gpu/depth_of_field.h), included with #include "depth_of_field.glsl": pure functions, mirrored by
// render/gpu/depth_of_field.cpp, which explains each, and checked against it by the GPU tests. The passes' loops are in
// depth_of_field_focus.comp, depth_of_field_prepare.comp, depth_of_field_neighbours.comp, depth_of_field.comp (the
// gather, which depthOfFieldAt mirrors), depth_of_field_median.comp and depth_of_field_composite.comp.

#ifndef DEPTH_OF_FIELD_GLSL
#define DEPTH_OF_FIELD_GLSL

#include "render/gpu/depth_of_field_shared.h"

float blurRadius(float inverseDistance, float inverseFocus, float focalLength, float lensScale) {
    float radius = lensScale * (inverseFocus - inverseDistance) / (1.0 - focalLength * inverseFocus);
    return clamp(radius, -float(DEPTH_OF_FIELD_LARGEST_RADIUS), float(DEPTH_OF_FIELD_LARGEST_RADIUS));
}

float easeFocus(float current, float target, float seconds, float timeConstant) {
    if (timeConstant <= 0.0) {
        return target;
    }

    return target + (current - target) * exp(-seconds / timeConstant);
}

float blurCoverage(float radius, float distance) {
    return clamp(abs(radius) - distance + float(DEPTH_OF_FIELD_EDGE), 0.0, 1.0);
}

vec2 blurRingOffset(int ring, int step) {
    float turn = (float(step) + ((ring % 2) == 1 ? 0.5 : 0.0)) / float(8 * ring);
    float angle = turn * 6.28318530717959;
    float distance = (float(ring) - 0.5) / float(DEPTH_OF_FIELD_RINGS);
    return vec2(cos(angle), sin(angle)) * distance;
}

float blurRingShare(int ring) {
    float rings = float(DEPTH_OF_FIELD_RINGS);

    if (ring <= 1) {
        return 1.0 / (9.0 * rings * rings);
    }

    float k = float(ring);
    return (2.0 * k - 1.0) / (8.0 * k * rings * rings);
}

float farSampleWeight(float sampleRadius, float pixelRadius, float distance) {
    float inFocus = float(DEPTH_OF_FIELD_IN_FOCUS);

    if (sampleRadius < -inFocus) {
        return 0.0;
    }

    float reach = pixelRadius < -inFocus ? float(DEPTH_OF_FIELD_LARGEST_RADIUS) : max(pixelRadius, 0.0);
    return blurCoverage(min(max(sampleRadius, 0.0), reach), distance);
}

float nearSampleWeight(float sampleRadius, float distance, float reach) {
    if (sampleRadius >= -float(DEPTH_OF_FIELD_IN_FOCUS)) {
        return 0.0;
    }

    return blurCoverage(sampleRadius, distance) * reach * reach / max(sampleRadius * sampleRadius, 1.0);
}

/*
* Each component's median of nine, by McGuire 2008's exchange network ("A Fast, Small-Radius GPU Median Filter"): each
* step drops the smallest and largest of the values left that can no longer be the median, so it needs no sort.
*/
void depthOfFieldSwap(inout vec4 a, inout vec4 b) {
    vec4 low = min(a, b);
    b = max(a, b);
    a = low;
}

vec4 medianOfNine(vec4 v[9]) {
    // mnmx6(0, 1, 2, 3, 4, 5)
    depthOfFieldSwap(v[0], v[3]);
    depthOfFieldSwap(v[1], v[4]);
    depthOfFieldSwap(v[2], v[5]);
    depthOfFieldSwap(v[0], v[1]);
    depthOfFieldSwap(v[0], v[2]);
    depthOfFieldSwap(v[4], v[5]);
    depthOfFieldSwap(v[3], v[5]);
    // mnmx5(1, 2, 3, 4, 6)
    depthOfFieldSwap(v[1], v[2]);
    depthOfFieldSwap(v[3], v[4]);
    depthOfFieldSwap(v[1], v[3]);
    depthOfFieldSwap(v[1], v[6]);
    depthOfFieldSwap(v[4], v[6]);
    depthOfFieldSwap(v[2], v[6]);
    // mnmx4(2, 3, 4, 7)
    depthOfFieldSwap(v[2], v[3]);
    depthOfFieldSwap(v[4], v[7]);
    depthOfFieldSwap(v[2], v[4]);
    depthOfFieldSwap(v[3], v[7]);
    // mnmx3(3, 4, 8)
    depthOfFieldSwap(v[4], v[8]);
    depthOfFieldSwap(v[3], v[8]);
    depthOfFieldSwap(v[3], v[4]);
    return v[4];
}

vec3 depthOfFieldComposite(vec3 sharp, float radius, vec3 far, vec3 nearColour, float nearAlpha) {
    float blurred = clamp(abs(radius) / float(DEPTH_OF_FIELD_IN_FOCUS) - 1.0, 0.0, 1.0);
    return mix(mix(sharp, far, blurred), nearColour, nearAlpha);
}

#endif
