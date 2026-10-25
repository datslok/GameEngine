// Shaping a deforming mesh on the GPU (cloth_shape.comp): the steps of scene/deforming_mesh.cpp's deformVertices,
// which mirrors them (triangleAreaNormal, triangleTangent, deformedNormal, deformedTangent, blendedPositions), checked
// against it (tests/gpu/cloth_gpu_test.cpp). Pure functions.

#ifndef DEFORM_GLSL
#define DEFORM_GLSL

#include "scene/deforming_mesh_shared.h"

// A point between the last two ticks, alpha of the way from the earlier.
vec3 deformBlend(vec3 previous, vec3 current, float alpha) {
    return previous + (current - previous) * alpha;
}

// The triangle's normal times twice its area.
vec3 deformAreaNormal(vec3 first, vec3 second, vec3 third) {
    return cross(second - first, third - first);
}

// The triangle's direction of increasing u in xyz, times the sign of its UV area; w 1 when its UVs have area, else 0.
vec4 deformTangent(vec3 first, vec3 second, vec3 third, vec2 firstUv, vec2 secondUv, vec2 thirdUv) {
    vec3 edge1 = second - first;
    vec3 edge2 = third - first;
    vec2 uvEdge1 = secondUv - firstUv;
    vec2 uvEdge2 = thirdUv - firstUv;
    float uvArea = uvEdge1.x * uvEdge2.y - uvEdge2.x * uvEdge1.y;
    vec3 alongU = (edge1 * uvEdge2.y - edge2 * uvEdge1.y) * (uvArea < 0.0 ? -1.0 : 1.0);
    return vec4(alongU, uvArea != 0.0 ? 1.0 : 0.0);
}

// A summed normal made unit, or straight up for none.
vec3 deformNormal(vec3 summed) {
    return dot(summed, summed) > DEFORM_SMALLEST_DIRECTION ? summed / length(summed) : vec3(0.0, 1.0, 0.0);
}

// A summed tangent made square to the unit normal and unit, or any direction square to it for none.
vec3 deformTangentDirection(vec3 summed, vec3 normal) {
    vec3 square = summed - normal * dot(normal, summed);
    if (dot(square, square) > DEFORM_SMALLEST_DIRECTION) {
        return square / length(square);
    }
    vec3 axis = abs(normal.x) < DEFORM_PERPENDICULAR_SWITCH ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 across = cross(normal, axis);
    return across / length(across);
}

#endif
