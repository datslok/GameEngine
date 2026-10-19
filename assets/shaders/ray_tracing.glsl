#ifndef RAY_TRACING_GLSL
#define RAY_TRACING_GLSL

// Ray tracing's pure maths (render/gpu/ray_tracing.h mirrors it): the camera's ray through a point on screen, and a hit's
// corner weights. No resources here, so the GPU tests run it through the function runner.

#include "render/gpu/ray_tracing_shared.h"

// The direction (not normalised) through a point on screen, x and y from -1 (left, bottom) to 1; right and up are
// already scaled by the tangents of half the view's width and height.
vec3 cameraRayDirection(vec3 right, vec3 up, vec3 forward, float x, float y) {
    return forward + right * x + up * y;
}

// A hit's barycentric coordinates are the second and third corners' weights; the first gets the rest.
vec3 hitCornerWeights(float second, float third) {
    return vec3(1.0 - second - third, second, third);
}

#endif
