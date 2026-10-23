// Ambient occlusion (render/gpu/ambient_occlusion.h): ground-truth ambient occlusion (Jimenez et al. 2016) from the
// depth image. Pure functions, mirrored by render/gpu/ambient_occlusion.cpp, which explains each, and checked against it
// by the GPU tests; the walk through the depth image is in ambient_occlusion.comp (ambientOcclusionAt mirrors it).

#ifndef AMBIENT_OCCLUSION_GLSL
#define AMBIENT_OCCLUSION_GLSL

#include "render/gpu/ambient_occlusion_shared.h"

// An occluder's weight at this distance: 1 up to the falloff's start, 0 at the radius.
float occluderWeight(float distance, float radius, float falloff) {
    return clamp((radius - distance) / (falloff * radius), 0.0, 1.0);
}

// A slice through the point: its axis, the normal's angle from the way to the camera in it, the projected length.
struct AoSlice {
    vec3 axis;
    float normalAngle;
    float projectedLength;
};

AoSlice aoSlice(vec3 normal, vec3 toCamera, vec3 direction) {
    vec3 across = direction - toCamera * dot(direction, toCamera);
    vec3 axis = normalize(cross(across, toCamera));
    vec3 projected = normal - axis * dot(normal, axis);
    float projectedLength = length(projected);

    if (!(projectedLength > 0.0)) {
        return AoSlice(axis, 0.0, 0.0);
    }

    float cosine = clamp(dot(projected, toCamera) / projectedLength, 0.0, 1.0);
    float side = dot(across, projected) < 0.0 ? -1.0 : 1.0;
    return AoSlice(axis, side * acos(cosine), projectedLength);
}

// The horizon's cosine with nothing above the surface: side 0 (along the slice's direction) or 1.
float lowHorizonCosine(AoSlice slice, int side) {
    const float halfTurn = 1.5707963;
    return cos(side == 0 ? slice.normalAngle + halfTurn : slice.normalAngle - halfTurn);
}

float horizonCosine(vec3 offset, vec3 toCamera) {
    return dot(offset, toCamera) / length(offset);
}

// The cosine-weighted open arc between the horizons, times the projected normal's length.
float sliceVisibility(AoSlice slice, float horizonCosine0, float horizonCosine1) {
    const float halfTurn = 1.5707963;
    float n = slice.normalAngle;
    float h0 = n + max(-acos(clamp(horizonCosine1, -1.0, 1.0)) - n, -halfTurn);
    float h1 = n + min(acos(clamp(horizonCosine0, -1.0, 1.0)) - n, halfTurn);
    float cosine = cos(n);
    float sine = sin(n);
    float arc0 = (cosine + 2.0 * h0 * sine - cos(2.0 * h0 - n)) * 0.25;
    float arc1 = (cosine + 2.0 * h1 * sine - cos(2.0 * h1 - n)) * 0.25;
    return slice.projectedLength * (arc0 + arc1);
}

// The step along the surface each way across the screen: the side whose next neighbour carries on in line with it.
vec3 surfaceStep(vec3 centre, vec3 before, vec3 beforeNext, vec3 after, vec3 afterNext) {
    vec3 fromBefore = centre - before;
    vec3 toAfter = after - centre;

    if (!(dot(fromBefore, fromBefore) > 0.0)) {
        return toAfter;
    }

    if (!(dot(toAfter, toAfter) > 0.0)) {
        return fromBefore;
    }

    vec3 missBefore = centre - (before * 2.0 - beforeNext);
    vec3 missAfter = centre - (after * 2.0 - afterNext);
    return dot(missAfter, missAfter) < dot(missBefore, missBefore) ? toAfter : fromBefore;
}

// The unit normal from the steps across and down, facing the camera at the origin.
vec3 depthNormal(vec3 centre, vec3 across, vec3 downwards) {
    vec3 normal = cross(downwards, across);
    float normalLength = length(normal);

    if (!(normalLength > 0.0)) {
        return normalize(-centre);
    }

    vec3 unit = normal * (1.0 / normalLength);
    return dot(unit, centre) > 0.0 ? -unit : unit;
}

float aoScreenRadius(float radius, float footprint, float viewDistance) {
    return min(radius / (footprint * viewDistance), float(AO_MAX_SCREEN_RADIUS));
}

float aoSampleDistance(int index, int count, float noise, float screenRadius) {
    float share = (float(index) + noise) / float(count);
    return max(share * share * screenRadius, float(AO_MIN_SAMPLE_PIXELS));
}

// Jimenez et al. 2016's multiple bounces: never darker than the visibility.
vec3 multiBounceOcclusion(float visibility, vec3 albedo) {
    vec3 a = float(AO_BOUNCE_A_SLOPE) * albedo + float(AO_BOUNCE_A_OFFSET);
    vec3 b = float(AO_BOUNCE_B_SLOPE) * albedo + float(AO_BOUNCE_B_OFFSET);
    vec3 c = float(AO_BOUNCE_C_SLOPE) * albedo + float(AO_BOUNCE_C_OFFSET);
    return max(vec3(visibility), ((visibility * a + b) * visibility + c) * visibility);
}

// Lagarde and de Rousiers 2014's specular occlusion.
float specularOcclusion(float nDotV, float visibility, float roughness) {
    return clamp(pow(nDotV + visibility, exp2(-16.0 * roughness - 1.0)) - 1.0 + visibility, 0.0, 1.0);
}

float aoBlurWeight(float centreDistance, float neighbourDistance) {
    return max(1.0 - abs(neighbourDistance - centreDistance) / (float(AO_BLUR_DEPTH_SHARE) * centreDistance), 0.0);
}

#endif
