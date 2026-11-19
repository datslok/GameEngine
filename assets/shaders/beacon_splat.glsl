#ifndef BEACON_SPLAT_GLSL
#define BEACON_SPLAT_GLSL

// Drawing beacons. Pure functions, mirrored by render/gpu/beacon_splat.h, which explains each; the GPU tests check them
// against it.
#include "render/gpu/beacon_splat_shared.h"
#include "stars.glsl"

const float BEACON_PI = 3.14159265358979;

float beaconLensPixels(float lensRadius, float distance, float pixelSolidAngle) {
    return lensRadius / max(distance, BEACON_NEAREST_DISTANCE) / sqrt(pixelSolidAngle);
}

float beaconSplatRadiance(float intensity, float distance, float pixelSolidAngle) {
    float far = max(distance, BEACON_NEAREST_DISTANCE);
    return intensity / (far * far * pixelSolidAngle);
}

float beaconSplatWeight(vec2 offset, float lensPixels) {
    float toDisc = clamp(lensPixels - 1.0, 0.0, 1.0);
    float radius = max(lensPixels, 1.0);
    float cover = clamp(radius + 0.5 - length(offset), 0.0, 1.0);
    float disc = cover / (BEACON_PI * radius * radius);
    return (1.0 - toDisc) * starSplatWeight(offset) + toDisc * disc;
}

float beaconExtentPixels(float lensPixels) {
    return max(lensPixels, 1.0) + 1.0;
}

#endif
