#version 450
#extension GL_GOOGLE_include_directive : require

// The curves, bloom's mix and the dither, shared with the GPU tests (tests/gpu) and mirrored in C++.
#include "tone_mapping.glsl"
#include "bloom.glsl"
#include "dither.glsl"

// The lit scene, in unbounded linear brightness (16-bit floats), the same size as the screen.
layout(set = 2, binding = 0) uniform sampler2D sceneColour;

// The grown bloom chain's level 0 (half the screen's size), read with a linear sampler. Only read when bloom is on.
layout(set = 2, binding = 1) uniform sampler2D bloom;

// Must match ToneMapUniformData in render/gpu/tone_map_uniforms.h.
layout(std140, set = 3, binding = 0) uniform ToneMapData {
    vec4 parameters; // x: exposure, y: peak brightness (in units of SDR white), z: curve (a TONE_MAP_ number),
                     // w: bloom strength (0 = no bloom)
    vec4 display;    // x: what SDR white is written as (1, or an HDR screen's white level), y: 1 to dither,
                     // z: what the bloom is multiplied by (1 / its level count)
};

layout(location = 0) out vec4 outputColour;

/*
* The final pass: each screen pixel reads the same pixel of the lit scene, mixes in the scattered light (bloom: a
* strength's share of the light replaced by the blurred scene, so no light is added), scales it by the exposure,
* measures it in units of the screen's peak, curves it into 0..1 and scales back. An 8-bit screen then gets the dither (it belongs
* here, at the last step: the scene image keeps 16-bit floats, which do not band). An HDR screen takes a float image in
* scRGB, where 1 is a dim 80 nits, so the result is scaled until white sits at the white level Windows uses for the
* desktop; the peak is the screen's headroom above that white.
*/
void main() {
    float exposure = parameters.x;
    float peak = parameters.y;
    int curve = int(parameters.z + 0.5);

    vec3 scene = max(texelFetch(sceneColour, ivec2(gl_FragCoord.xy), 0).rgb, vec3(0.0));
    float bloomStrength = parameters.w;

    if (bloomStrength > 0.0) {
        vec2 place = gl_FragCoord.xy / vec2(textureSize(sceneColour, 0));
        scene = mixBloom(scene, texture(bloom, place).rgb * display.z, bloomStrength);
    }
    vec3 mapped = toneMap(scene, exposure, peak, curve);

    if (display.y > 0.5) {
        mapped = ditherForEightBits(mapped, interleavedGradientNoise(gl_FragCoord.xy));
    }

    outputColour = vec4(mapped * display.x, 1.0);
}
