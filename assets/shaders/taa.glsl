// Temporal anti-aliasing's maths, included with #include "taa.glsl": mirrors render/gpu/taa.cpp, which explains each
// function and tests the maths, and tests/gpu checks these functions against it on the GPU. Pure functions only.

#ifndef TAA_GLSL
#define TAA_GLSL

#include "render/gpu/taa_shared.h"

// karisWeight, for the blend's weights.
#include "bloom.glsl"

float halton(uint index, uint base) {
    float result = 0.0;
    float fraction = 1.0;

    while (index > 0u) {
        fraction /= float(base);
        result += fraction * float(index % base);
        index /= base;
    }

    return result;
}

vec2 taaJitter(uint frame) {
    uint index = frame % uint(TAA_JITTER_FRAMES) + 1u;
    return vec2(halton(index, 2u) - 0.5, halton(index, 3u) - 0.5);
}

vec2 ndcToPixel(vec2 ndc, vec2 size) {
    return vec2((ndc.x + 1.0) * 0.5 * size.x, (1.0 - ndc.y) * 0.5 * size.y);
}

vec2 pixelToNdc(vec2 pixel, vec2 size) {
    return vec2(pixel.x / size.x * 2.0 - 1.0, 1.0 - pixel.y / size.y * 2.0);
}

// Last frame's pixel (xy) and 1 when it was in front of that camera (z), as reprojectPixel's Reprojected.
vec3 reprojectPixel(mat4 reprojection, vec2 pixel, float depth, vec2 size) {
    vec2 ndc = pixelToNdc(pixel, size);
    vec4 clip = reprojection * vec4(ndc, depth, 1.0);

    if (!(clip.w > 0.0)) {
        return vec3(0.0);
    }

    return vec3(ndcToPixel(clip.xy / clip.w, size), 1.0);
}

vec2 previousPixelFromMotion(vec2 pixel, vec2 motion, vec2 size) {
    return vec2(pixel.x - motion.x * 0.5 * size.x, pixel.y + motion.y * 0.5 * size.y);
}

vec2 objectMotion(vec4 currentClip, vec4 previousClip) {
    if (!(previousClip.w > 0.0) || !(currentClip.w > 0.0)) {
        return vec2(TAA_NO_MOTION);
    }

    return currentClip.xy / currentClip.w - previousClip.xy / previousClip.w;
}

vec2 sampleOutputPixel(vec2 renderPixel, vec2 jitter, vec2 outputPerRender) {
    return (renderPixel - jitter) * outputPerRender;
}

vec2 renderPixelOf(vec2 outputPixel, vec2 renderPerOutput) {
    return outputPixel * renderPerOutput;
}

float taaSampleWeight(float distanceSquared) {
    return exp(-float(TAA_SAMPLE_FALLOFF) * distanceSquared);
}

float taaCurrentShare(float currentWeight, float nearestSampleWeight) {
    return currentWeight * nearestSampleWeight;
}

vec3 toYCoCg(vec3 rgb) {
    return vec3(0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b, 0.5 * rgb.r - 0.5 * rgb.b, -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b);
}

vec3 fromYCoCg(vec3 yCoCg) {
    return vec3(yCoCg.x + yCoCg.y - yCoCg.z, yCoCg.x + yCoCg.z, yCoCg.x - yCoCg.y - yCoCg.z);
}

vec3 clipToBox(vec3 history, vec3 centre, vec3 extent) {
    const float thinnest = 1e-7;
    vec3 offset = history - centre;
    vec3 reaches = abs(offset) / max(extent, vec3(thinnest));
    float reach = max(reaches.x, max(reaches.y, reaches.z));
    return reach > 1.0 ? centre + offset * (1.0 / reach) : history;
}

vec3 taaBlend(vec3 current, vec3 history, float currentWeight) {
    float currentShare = currentWeight * karisWeight(current);
    float historyShare = (1.0 - currentWeight) * karisWeight(history);
    return (current * currentShare + history * historyShare) * (1.0 / (currentShare + historyShare));
}

vec4 catmullRomWeights(float t) {
    return vec4(t * (-0.5 + t * (1.0 - 0.5 * t)), 1.0 + t * t * (-2.5 + 1.5 * t), t * (0.5 + t * (2.0 - 1.5 * t)), t * t * (-0.5 + 0.5 * t));
}

#endif
