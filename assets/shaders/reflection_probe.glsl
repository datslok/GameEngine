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

float parallaxStep(vec3 position, vec3 direction, float storedDistance) {
    float along = dot(position, direction);
    float discriminant = along * along - dot(position, position) + storedDistance * storedDistance;

    if (discriminant < 0.0) {
        return -1.0;
    }

    float t = -along + sqrt(discriminant);
    return t >= 0.0 ? t : -1.0;
}

vec3 parallaxDirection(vec3 position, vec3 direction, float t) {
    return t >= 0.0 ? normalize(position + direction * t) : direction;
}

float parallaxConfidence(float reached, float stored) {
    float mismatch = abs(reached - stored) / max(stored, 1e-6);
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
