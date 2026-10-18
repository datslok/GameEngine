#version 450
#extension GL_GOOGLE_include_directive : require

// The curves' numbers, shared with render/gpu/tone_mapping.cpp, which mirrors this shader and tests the maths.
#include "render/gpu/tone_mapping_shared.h"
#include "dither.glsl"

// The lit scene, in unbounded linear brightness (16-bit floats), the same size as the screen.
layout(set = 2, binding = 0) uniform sampler2D sceneColour;

// Must match ToneMapUniformData in render/gpu/tone_map_uniforms.h.
layout(std140, set = 3, binding = 0) uniform ToneMapData {
    vec4 parameters; // x: exposure, y: peak brightness (in units of SDR white), z: curve (a TONE_MAP_ number)
    vec4 display;    // x: what SDR white is written as (1, or an HDR screen's white level), y: 1 to dither
};

layout(location = 0) out vec4 outputColour;

vec3 reinhard(vec3 value) {
    return value / (1.0 + value);
}

vec3 aces(vec3 value) {
    return (value * (ACES_A * value + ACES_B)) / (value * (ACES_C * value + ACES_D) + ACES_E);
}

// The S-curve over the stops range, t in 0..1, by Horner's rule.
vec3 agxContrast(vec3 t) {
    vec3 result = vec3(AGX_C6);
    result = result * t + AGX_C5;
    result = result * t + AGX_C4;
    result = result * t + AGX_C3;
    result = result * t + AGX_C2;
    result = result * t + AGX_C1;
    return result * t + AGX_C0;
}

// AgX: mix the channels slightly, curve them in stops (a log scale), unmix and undo the display gamma.
// GLSL builds matrices column by column, so the row-by-row numbers are listed down each column.
vec3 agx(vec3 value) {
    const mat3 inset = mat3(
        AGX_INSET_00, AGX_INSET_10, AGX_INSET_20,
        AGX_INSET_01, AGX_INSET_11, AGX_INSET_21,
        AGX_INSET_02, AGX_INSET_12, AGX_INSET_22);
    const mat3 outset = mat3(
        AGX_OUTSET_00, AGX_OUTSET_10, AGX_OUTSET_20,
        AGX_OUTSET_01, AGX_OUTSET_11, AGX_OUTSET_21,
        AGX_OUTSET_02, AGX_OUTSET_12, AGX_OUTSET_22);

    vec3 stops = clamp(log2(max(inset * value, vec3(1e-10))), AGX_MIN_STOPS, AGX_MAX_STOPS);
    vec3 encoded = outset * agxContrast((stops - AGX_MIN_STOPS) / (AGX_MAX_STOPS - AGX_MIN_STOPS));
    return pow(clamp(encoded, 0.0, 1.0), vec3(AGX_DISPLAY_GAMMA));
}

// The curve for a screen whose peak is 1.
vec3 applyCurve(int curve, vec3 value) {
    if (curve == TONE_MAP_REINHARD) {
        return reinhard(value);
    }

    if (curve == TONE_MAP_ACES) {
        return aces(value);
    }

    if (curve == TONE_MAP_AGX) {
        return agx(value);
    }

    return min(value, vec3(1.0));
}

/*
* The final pass: each screen pixel reads the same pixel of the lit scene, scales it by the exposure, measures it in
* units of the screen's peak, curves it into 0..1 and scales back. An 8-bit screen then gets the dither (it belongs
* here, at the last step: the scene image keeps 16-bit floats, which do not band). An HDR screen takes a float image in
* scRGB, where 1 is a dim 80 nits, so the result is scaled until white sits at the white level Windows uses for the
* desktop; the peak is the screen's headroom above that white.
*/
void main() {
    float exposure = parameters.x;
    float peak = parameters.y;
    int curve = int(parameters.z + 0.5);

    vec3 scene = max(texelFetch(sceneColour, ivec2(gl_FragCoord.xy), 0).rgb, vec3(0.0));
    vec3 mapped = clamp(applyCurve(curve, scene * (exposure / peak)), 0.0, 1.0) * peak;

    if (display.y > 0.5) {
        mapped = ditherForEightBits(mapped);
    }

    outputColour = vec4(mapped * display.x, 1.0);
}
