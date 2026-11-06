// Planar reflections (render/gpu/planar_reflection.h). Pure functions, mirrored by render/gpu/planar_reflection.cpp,
// which explains each, and checked against it by the GPU tests.

#ifndef PLANAR_REFLECTION_GLSL
#define PLANAR_REFLECTION_GLSL

#include "render/gpu/planar_reflection_shared.h"

// 1 if a point (from the camera) with this normal is on the mirror's plane, as measured from the camera, else 0.
float isOnPlanarMirror(vec3 position, vec3 normal, vec3 planeNormal, float planeOffset) {
    float tolerance = float(PLANAR_PLANE_TOLERANCE) + float(PLANAR_PLANE_TOLERANCE_PER_METRE) * length(position);
    return abs(dot(planeNormal, position) - planeOffset) <= tolerance && dot(normal, planeNormal) >= float(PLANAR_NORMAL_COSINE) ? 1.0 : 0.0;
}

#endif
