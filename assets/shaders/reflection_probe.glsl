// The reflection probe's maths, included with #include "reflection_probe.glsl": mirrors render/gpu/reflection_probe.cpp,
// which explains each function and tests the maths, and tests/gpu checks these functions against it on the GPU. Pure
// functions only: the parallax search's loop, which reads the probe, is in reflections.comp (mirrored by searchProbe),
// and the prefilter's in probe_prefilter.comp (mirrored by prefilterTexel).

#ifndef REFLECTION_PROBE_GLSL
#define REFLECTION_PROBE_GLSL

#include "render/gpu/reflection_probe_shared.h"

// A face's way, the way its picture's columns run, and its up (rows count down), in Vulkan's cube order.
void cubeFaceAxes(int face, out vec3 forward, out vec3 right, out vec3 up) {
    if (face == 0) {
        forward = vec3(1.0, 0.0, 0.0); right = vec3(0.0, 0.0, -1.0); up = vec3(0.0, 1.0, 0.0);
    } else if (face == 1) {
        forward = vec3(-1.0, 0.0, 0.0); right = vec3(0.0, 0.0, 1.0); up = vec3(0.0, 1.0, 0.0);
    } else if (face == 2) {
        forward = vec3(0.0, 1.0, 0.0); right = vec3(1.0, 0.0, 0.0); up = vec3(0.0, 0.0, -1.0);
    } else if (face == 3) {
        forward = vec3(0.0, -1.0, 0.0); right = vec3(1.0, 0.0, 0.0); up = vec3(0.0, 0.0, 1.0);
    } else if (face == 4) {
        forward = vec3(0.0, 0.0, 1.0); right = vec3(1.0, 0.0, 0.0); up = vec3(0.0, 1.0, 0.0);
    } else {
        forward = vec3(0.0, 0.0, -1.0); right = vec3(-1.0, 0.0, 0.0); up = vec3(0.0, 1.0, 0.0);
    }
}

vec3 cubeTexelDirection(int face, vec2 st) {
    vec3 forward;
    vec3 right;
    vec3 up;
    cubeFaceAxes(face, forward, right, up);
    return forward + right * (2.0 * st.x - 1.0) - up * (2.0 * st.y - 1.0);
}

float probeRadialDistance(float viewDistance, vec3 unitDirection) {
    vec3 size = abs(unitDirection);
    return viewDistance / max(size.x, max(size.y, size.z));
}

float parallaxMismatch(vec3 position, vec3 direction, float t, float storedDistance) {
    return length(position + direction * t) - storedDistance;
}

float probeTexelAngle(float size) {
    return 0.5 * 3.14159265358979 / size;
}

float probeStartLift(vec3 position, vec3 normal, float size) {
    float distance = length(position);
    float facing = distance > 0.0 ? dot(normal, -position / distance) : 1.0;
    float texels = float(PROBE_START_LIFT_TEXELS) * probeTexelAngle(size) * distance;
    return texels / max(facing, float(PROBE_START_LIFT_SLANT));
}

float probeEdgeConfidence(float spread) {
    return 1.0 - smoothstep(float(PROBE_EDGE_JUMP_START), float(PROBE_EDGE_JUMP_END), spread);
}

// How far four texels' distances spread, as a share of the farthest (a textureGather's four).
float probeDistanceSpread(vec4 texels) {
    float farthest = max(max(texels.x, texels.y), max(texels.z, texels.w));
    float nearest = min(min(texels.x, texels.y), min(texels.z, texels.w));
    return (farthest - nearest) / max(farthest, 1e-6);
}

float probeViewConfidence(float angle, float size) {
    float texel = probeTexelAngle(size);
    return 1.0 - smoothstep(texel, 2.0 * texel, angle);
}

// The cell (face, column, row) of a level with side cells a side that a direction falls in: the face of its largest
// component, x before y before z on a tie, as Vulkan picks it.
ivec3 cubeCellOf(vec3 direction, int side) {
    vec3 size = abs(direction);
    int face = size.x >= size.y && size.x >= size.z ? (direction.x >= 0.0 ? 0 : 1)
             : size.y >= size.z                     ? (direction.y >= 0.0 ? 2 : 3)
                                                    : (direction.z >= 0.0 ? 4 : 5);
    vec3 forward;
    vec3 right;
    vec3 up;
    cubeFaceAxes(face, forward, right, up);
    float major = dot(forward, direction);
    vec2 st = 0.5 * (vec2(dot(right, direction), -dot(up, direction)) / major + 1.0);
    ivec2 cell = clamp(ivec2(st * float(side)), ivec2(0), ivec2(side - 1));
    return ivec3(face, cell);
}

float cubeCellExit(vec3 position, vec3 direction, float from, ivec3 cell, int side, float end) {
    vec3 forward;
    vec3 right;
    vec3 up;
    cubeFaceAxes(cell.x, forward, right, up);
    float across = float(side);
    float edges[4] = float[4](2.0 * float(cell.y) / across - 1.0, 2.0 * float(cell.y + 1) / across - 1.0,
                              2.0 * float(cell.z) / across - 1.0, 2.0 * float(cell.z + 1) / across - 1.0);
    float exit = end;

    for (int edge = 0; edge < 4; ++edge) {
        vec3 normal = edge < 2 ? right - forward * edges[edge] : -up - forward * edges[edge];
        float approach = dot(direction, normal);

        if (abs(approach) < 1e-12) {
            continue;
        }

        float t = -dot(position, normal) / approach;

        if (t > from && t < exit && dot(forward, position + direction * t) > 0.0) {
            exit = t;
        }
    }

    if (exit >= end) {
        return end;
    }

    return min(exit + max(exit * float(PROBE_CELL_CROSSING_SHARE), float(PROBE_CELL_CROSSING)), end);
}

vec3 parallaxDirection(vec3 position, vec3 direction, float t) {
    return t >= 0.0 ? normalize(position + direction * t) : direction;
}

float parallaxConfidence(float nearSide, float farSide) {
    float mismatch = abs(nearSide - farSide) / max(max(nearSide, farSide), 1e-6);
    return 1.0 - smoothstep(float(PROBE_MATCH_SHARE), float(PROBE_MISMATCH_SHARE), mismatch);
}

float probeRoughnessLevel(float roughness, float levels) {
    return clamp(roughness, 0.0, 1.0) * (levels - 1.0);
}

float prefilterSourceLevel(float pdf, float samples, float size) {
    float sampleAngle = 1.0 / (samples * pdf);
    float texelAngle = 4.0 * 3.14159265358979 / (6.0 * size * size);
    return max(0.5 * log2(sampleAngle / texelAngle) + 1.0, 0.0);
}

vec3 blendedStandIn(vec3 standIn, vec3 specularAlbedo, vec3 light, float confidence) {
    return standIn + (specularAlbedo * light - standIn) * confidence;
}

#endif
