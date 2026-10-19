// Soft shadows' arithmetic, included with #include "soft_shadow.glsl": mirrors render/gpu/soft_shadow.cpp, which tests
// it, and tests/gpu checks these functions against it on the GPU. Widths are in texels of the tile.

#ifndef SOFT_SHADOW_GLSL
#define SOFT_SHADOW_GLSL

#include "render/gpu/shader_shared.h"

const float minShadowFilterTexels = MIN_SHADOW_FILTER_TEXELS;
const float maxShadowFilterTexels = MAX_SHADOW_FILTER_TEXELS;
const float maxBlockerSearchTexels = MAX_BLOCKER_SEARCH_TEXELS;

float linearShadowDepth(float depth, float nearPlane, float farPlane) {
    return farPlane * nearPlane / (nearPlane + depth * (farPlane - nearPlane));
}

float blockerSearchTexels(float emitterRadius, float receiverDepth, float nearPlane, float texelPerUnit) {
    float widthPerDistance = emitterRadius * (receiverDepth - nearPlane) / (nearPlane * receiverDepth);
    return clamp(widthPerDistance / texelPerUnit, minShadowFilterTexels, maxBlockerSearchTexels);
}

float penumbraTexels(float emitterRadius, float receiverDepth, float blockerDepth, float texelPerUnit) {
    float worldWidth = emitterRadius * max(receiverDepth - blockerDepth, 0.0) / blockerDepth;
    return clamp(worldWidth / (receiverDepth * texelPerUnit), minShadowFilterTexels, maxShadowFilterTexels);
}

float boxShadowDistance(float depth, float depthRange) {
    return (1.0 - depth) * depthRange;
}

float sunBlockerSearchTexels(float tanAngularRadius, float receiverDistance, float texelSize) {
    return clamp(tanAngularRadius * receiverDistance / texelSize, minShadowFilterTexels, maxBlockerSearchTexels);
}

float sunPenumbraTexels(float tanAngularRadius, float receiverDistance, float blockerDistance, float texelSize) {
    float worldWidth = tanAngularRadius * max(receiverDistance - blockerDistance, 0.0);
    return clamp(worldWidth / texelSize, minShadowFilterTexels, maxShadowFilterTexels);
}

#endif
