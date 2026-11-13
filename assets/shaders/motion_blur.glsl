// Motion blur (render/gpu/motion_blur.h), included with #include "motion_blur.glsl": pure functions, mirrored by
// render/gpu/motion_blur.cpp, which explains each, and checked against it by the GPU tests. The passes' loops are in
// motion_blur_tiles.comp, motion_blur_neighbours.comp and motion_blur.comp (motionBlurAt mirrors the last).

#ifndef MOTION_BLUR_GLSL
#define MOTION_BLUR_GLSL

#include "render/gpu/motion_blur_shared.h"
#include "render/gpu/taa_shared.h"

vec2 pixelMotion(vec2 velocityTexel, vec2 cameraMotionNdc, bool cameraMotionBlur) {
    bool objectMoving = !(velocityTexel.x > float(TAA_NO_MOTION_TEST));

    if (cameraMotionBlur) {
        return objectMoving ? velocityTexel : cameraMotionNdc;
    }

    return objectMoving ? velocityTexel - cameraMotionNdc : vec2(0.0);
}

vec2 motionBlurHalfVector(vec2 ndcMotion, vec2 outputSize, float shutterShare) {
    vec2 halfBlur = vec2(ndcMotion.x * 0.5 * outputSize.x, -ndcMotion.y * 0.5 * outputSize.y) * (shutterShare * 0.5);
    float blurLength = length(halfBlur);
    float longest = float(MOTION_BLUR_TILE);
    return blurLength > longest ? halfBlur * (longest / blurLength) : halfBlur;
}

vec2 largerMotion(vec2 a, vec2 b) {
    return dot(b, b) > dot(a, a) ? b : a;
}

float motionBlurCone(float distance, float speed) {
    return clamp(1.0 - distance / speed, 0.0, 1.0);
}

float motionBlurCylinder(float distance, float speed) {
    return 1.0 - smoothstep(0.95 * speed, 1.05 * speed, distance);
}

float motionBlurNearer(float a, float b) {
    return clamp(1.0 - (a - b) / (float(MOTION_BLUR_DEPTH_SOFTNESS) * b), 0.0, 1.0);
}

float motionBlurSampleWeight(float distance, float pixelDepth, float sampleDepth, float pixelSpeed, float sampleSpeed) {
    float inFront = motionBlurNearer(sampleDepth, pixelDepth);
    float behind = motionBlurNearer(pixelDepth, sampleDepth);
    return inFront * motionBlurCone(distance, sampleSpeed) + behind * motionBlurCone(distance, pixelSpeed) +
           motionBlurCylinder(distance, sampleSpeed) * motionBlurCylinder(distance, pixelSpeed) * 2.0;
}

float motionBlurOffset(int index, float noise) {
    float jitter = noise - 0.5;
    return -1.0 + 2.0 * (float(index) + jitter + 1.0) / float(MOTION_BLUR_SAMPLES + 1);
}

#endif
