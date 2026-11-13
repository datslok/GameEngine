// The camera's finish (render/gpu/camera_finish.h), included with #include "camera_finish.glsl": pure functions,
// mirrored by render/gpu/camera_finish.cpp, which explains each, and checked against it by the GPU tests. The white
// balance is a matrix the CPU builds (whiteBalanceMatrix); tone_map.frag multiplies by it.

#ifndef CAMERA_FINISH_GLSL
#define CAMERA_FINISH_GLSL

#include "render/gpu/camera_finish_shared.h"
#include "eye_adaptation.glsl"
#include "hash.glsl"

// grade: x contrast, y saturation, z lift, w gamma.
vec3 gradeColour(vec3 exposed, vec4 grade, float gain) {
    float midGrey = float(CAMERA_MID_GREY);
    vec3 colour = midGrey * pow(max(exposed, vec3(0.0)) / midGrey, vec3(grade.x));
    float luminance = pixelLuminance(colour);
    colour = max(luminance + (colour - luminance) * grade.y, vec3(0.0));
    return pow(max(colour * gain + grade.z, vec3(0.0)), vec3(1.0 / grade.w));
}

float naturalVignetting(vec2 ndc, vec2 tanHalfFov, float strength) {
    vec2 tangents = ndc * tanHalfFov;
    float tanSquared = dot(tangents, tangents);
    float cosFourth = 1.0 / ((1.0 + tanSquared) * (1.0 + tanSquared));
    return 1.0 + (cosFourth - 1.0) * strength;
}

vec2 aberrationPlace(vec2 pixel, vec2 size, float shiftPixels) {
    vec2 centre = size * 0.5;
    return centre + (pixel - centre) * (1.0 + shiftPixels / length(centre));
}

float grainNoise(uvec3 pixelAndFrame) {
    return float(pcg3d(pixelAndFrame).x >> 8u) * (1.0 / 16777216.0);
}

vec3 filmGrain(vec3 mapped, float noise, float strength, float peak) {
    float level = clamp(pixelLuminance(mapped) / peak, 0.0, 1.0);
    float grain = (noise - 0.5) * strength * 4.0 * level * (1.0 - level) * peak;
    return clamp(mapped + grain, vec3(0.0), vec3(peak));
}

#endif
