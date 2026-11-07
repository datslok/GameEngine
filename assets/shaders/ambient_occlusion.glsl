// Ambient occlusion (render/gpu/ambient_occlusion.h): ground-truth ambient occlusion (Jimenez et al. 2016) from the
// depth image. Pure functions, mirrored by render/gpu/ambient_occlusion.cpp, which explains each, and checked against it
// by the GPU tests; the walk through the depth image is in ambient_occlusion.comp (ambientOcclusionAt mirrors it).

#ifndef AMBIENT_OCCLUSION_GLSL
#define AMBIENT_OCCLUSION_GLSL

#include "render/gpu/ambient_occlusion_shared.h"
#include "taa.glsl"

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

// A gathered sample's light, brought down to the brightest it may be in its largest channel, keeping its colour.
vec3 gatheredSampleLight(vec3 light, float brightest) {
    float largest = max(max(light.r, light.g), light.b);
    return largest > brightest ? light * (brightest / largest) : light;
}

// The weight of the slice from the way to the camera out to angle h, either side.
float sliceArc(float normalAngle, float h) {
    return (cos(normalAngle) + 2.0 * h * sin(normalAngle) - cos(2.0 * h - normalAngle)) * 0.25;
}

// Where a direction lies in the slice as a share of its cosine-weighted light, 0 at side 1's horizon to 1 at side 0's.
float sectorShare(AoSlice slice, float directionCosine, int side) {
    const float halfTurn = 1.5707963;
    float n = slice.normalAngle;
    float low = n - halfTurn;
    float high = n + halfTurn;
    float angle = acos(clamp(directionCosine, -1.0, 1.0));
    float signedAngle = clamp(side == 0 ? angle : -angle, low, high);
    float arcLow = sliceArc(n, low);
    float arcHigh = sliceArc(n, high);
    float arc = sliceArc(n, signedAngle);
    return clamp(((signedAngle < 0.0 ? -arc : arc) + arcLow) / (arcLow + arcHigh), 0.0, 1.0);
}

// The sectors an arc between two shares covers at least half of, the shares either way round.
uint sectorMask(float share0, float share1) {
    int first = int(floor(min(share0, share1) * float(AO_SECTORS) + 0.5));
    int last = int(floor(max(share0, share1) * float(AO_SECTORS) + 0.5));
    int count = last - first;

    if (count <= 0) {
        return 0u;
    }

    if (count >= AO_SECTORS) {
        return 0xFFFFFFFFu;
    }

    return ((1u << count) - 1u) << first;
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

// The screen's visibility or the material's baked one, whichever is more closed (not their product).
float surfaceVisibility(float screenVisibility, float occlusionTexel, float occlusionStrength) {
    return min(screenVisibility, 1.0 + occlusionStrength * (occlusionTexel - 1.0));
}

// How much a sample at this offset counts towards the fill: nothing on or below the surface, 1 straight overhead.
float fillFacingWeight(vec3 normal, vec3 offset) {
    float offsetLength = length(offset);

    if (!(offsetLength > 0.0)) {
        return 0.0;
    }

    float lowest = float(AO_FILL_MIN_COSINE);
    return clamp((dot(normal, offset) / offsetLength - lowest) / (1.0 - lowest), 0.0, 1.0);
}

// The fill's light from its parts: the light above the surface over the coverage, never over less than the least.
vec3 aoFillLight(vec3 parts, float coverage) {
    return parts / max(coverage, float(AO_FILL_MIN_COVERAGE));
}

float aoBlurWeight(float centreDistance, float neighbourDistance) {
    return max(1.0 - abs(neighbourDistance - centreDistance) / (float(AO_BLUR_DEPTH_SHARE) * centreDistance), 0.0);
}

// Temporal accumulation: the age after one more frame, at most the cap.
float aoHistoryAge(float previousAge, int historyFrames) {
    return min(previousAge + 1.0, float(historyFrames));
}

// The new frame's share in the average at this age.
float aoCurrentShare(float age) {
    return 1.0 / max(age, 1.0);
}

// Whether a history texel shows the point: its view depth (0 for the sky) within share of the expected one.
bool aoHistoryTapKept(float expectedDepth, float storedDepth, float share) {
    return storedDepth > 0.0 && abs(storedDepth - expectedDepth) <= share * expectedDepth;
}

// Where the pixel's point was on last frame's screen: xy the pixel, z its view depth then, w 1 when it was in front of
// last frame's camera and on its screen.
vec4 aoPreviousPlace(mat4 reprojection, vec2 pixel, float depth, float viewDepth, vec2 size) {
    vec2 ndc = pixelToNdc(pixel, size);
    vec4 clip = reprojection * vec4(ndc, depth, 1.0);

    if (!(clip.w > 0.0)) {
        return vec4(0.0);
    }

    vec2 moved = clip.xy / clip.w - ndc;
    vec2 previous = vec2(pixel.x + moved.x * 0.5 * size.x, pixel.y - moved.y * 0.5 * size.y);

    if (any(lessThan(previous, vec2(0.0))) || any(greaterThan(previous, size))) {
        return vec4(0.0);
    }

    return vec4(previous, clip.w * viewDepth, 1.0);
}

// A bilinear tap's weight (tap 0 to 3: bit 0 the texel to the right, bit 1 the one below), from the fraction past the
// first texel's centre.
float aoTapWeight(vec2 fraction, int tap) {
    return ((tap & 1) == 1 ? fraction.x : 1.0 - fraction.x) * ((tap >> 1) == 1 ? fraction.y : 1.0 - fraction.y);
}

// The new frame blended into the history's light at the new age.
vec4 accumulatedAo(vec4 current, vec4 historyLight, float age) {
    return historyLight + (current - historyLight) * aoCurrentShare(age);
}

#endif
