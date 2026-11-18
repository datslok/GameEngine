// Occlusion culling's maths, included with #include "occlusion_culling.glsl": mirrors render/gpu/occlusion_culling.h
// (sphereRectFromView, nearestDepthFromView, occlusionLevel). Pure functions: the pyramid reads stay in the pass.
#ifndef OCCLUSION_CULLING_GLSL
#define OCCLUSION_CULLING_GLSL

#include "render/gpu/occlusion_culling_shared.h"

/*
* The pixels a sphere covers (centre in view space, the camera looking down -z), from its exact tangent planes (Mara &
* McGuire 2013), widened by OCCLUSION_RECT_MARGIN and clamped to the screen: rect = (min x, min y, max x, max y), empty
* when a min passes its max. False when the sphere reaches the near plane: it cannot be judged.
*/
bool occlusionSphereRect(vec3 viewCentre, float radius, vec2 scale, float nearPlane, ivec2 size, out ivec4 rect) {
    rect = ivec4(0, 0, -1, -1);
    float distance = -viewCentre.z;

    if (distance - radius <= nearPlane) {
        return false;
    }

    float lessRadius = distance * distance - radius * radius;
    float alongX = sqrt(viewCentre.x * viewCentre.x + lessRadius);
    float alongY = sqrt(viewCentre.y * viewCentre.y + lessRadius);
    float lowX = (alongX * viewCentre.x - radius * distance) / (alongX * distance + radius * viewCentre.x);
    float highX = (alongX * viewCentre.x + radius * distance) / (alongX * distance - radius * viewCentre.x);
    float lowY = (alongY * viewCentre.y - radius * distance) / (alongY * distance + radius * viewCentre.y);
    float highY = (alongY * viewCentre.y + radius * distance) / (alongY * distance - radius * viewCentre.y);

    vec2 extent = vec2(size);
    float beyond = 2.0 + float(OCCLUSION_RECT_MARGIN); // past the margin, so a sphere off the screen stays off it once widened
    float left = clamp((lowX * scale.x + 1.0) * 0.5 * extent.x, -beyond, extent.x + beyond);
    float right = clamp((highX * scale.x + 1.0) * 0.5 * extent.x, -beyond, extent.x + beyond);
    float top = clamp((1.0 - highY * scale.y) * 0.5 * extent.y, -beyond, extent.y + beyond);
    float bottom = clamp((1.0 - lowY * scale.y) * 0.5 * extent.y, -beyond, extent.y + beyond);

    rect.x = max(int(floor(left)) - OCCLUSION_RECT_MARGIN, 0);
    rect.y = max(int(floor(top)) - OCCLUSION_RECT_MARGIN, 0);
    rect.z = min(int(floor(right)) + OCCLUSION_RECT_MARGIN, size.x - 1);
    rect.w = min(int(floor(bottom)) + OCCLUSION_RECT_MARGIN, size.y - 1);
    return true;
}

// The GPU depth of the sphere's nearest point: depth = constant + perInverseDistance / distance (reversed-Z).
float occlusionNearestDepth(float viewZ, float radius, float depthConstant, float depthPerInverseDistance) {
    return depthConstant + depthPerInverseDistance / (-viewZ - radius);
}

// The pyramid level (below levels) at which the rectangle touches at most 2 x 2 texels.
int occlusionLevel(ivec4 rect, int levels) {
    int longest = max(rect.z - rect.x + 1, rect.w - rect.y + 1);
    int level = 0;

    while (level + 1 < levels && (1 << level) < longest) {
        ++level;
    }

    return level;
}

#endif
