#include "render/gpu/soft_shadow.h"

#include <algorithm>

float linearShadowDepth(float depth, float nearPlane, float farPlane) {
    return farPlane * nearPlane / (nearPlane + depth * (farPlane - nearPlane));
}

/*
* From the near plane, where blockers can sit furthest off to the side, up to the cap: an emitter big enough to need more
* would need more samples than the shader takes to look smooth anyway.
*/
float blockerSearchTexels(float emitterRadius, float receiverDepth, float nearPlane, float texelPerUnit) {
    const float widthPerDistance = emitterRadius * (receiverDepth - nearPlane) / (nearPlane * receiverDepth);
    return std::clamp(widthPerDistance / texelPerUnit, minShadowFilterTexels, maxBlockerSearchTexels);
}

float penumbraTexels(float emitterRadius, float receiverDepth, float blockerDepth, float texelPerUnit) {
    const float worldWidth = emitterRadius * std::max(receiverDepth - blockerDepth, 0.0f) / blockerDepth;
    const float texelWidth = receiverDepth * texelPerUnit;
    return std::clamp(worldWidth / texelWidth, minShadowFilterTexels, maxShadowFilterTexels);
}

float boxShadowDistance(float depth, float depthRange) {
    return (1.0f - depth) * depthRange;
}

float sunBlockerSearchTexels(float tanAngularRadius, float receiverDistance, float texelSize) {
    return std::clamp(tanAngularRadius * receiverDistance / texelSize, minShadowFilterTexels, maxBlockerSearchTexels);
}

float sunPenumbraTexels(float tanAngularRadius, float receiverDistance, float blockerDistance, float texelSize) {
    const float worldWidth = tanAngularRadius * std::max(receiverDistance - blockerDistance, 0.0f);
    return std::clamp(worldWidth / texelSize, minShadowFilterTexels, maxShadowFilterTexels);
}
