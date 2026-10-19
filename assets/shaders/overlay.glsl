// The debug UI overlay's colour arithmetic, included with #include "overlay.glsl": mirrors overlayOutput in
// render/gpu/overlay_composite.cpp, which tests the maths, and tests/gpu checks it against it on the GPU.

#ifndef OVERLAY_GLSL
#define OVERLAY_GLSL

#include "dither.glsl"

/*
* An overlay texel (8-bit, premultiplied, sRGB-encoded as ImGui wrote it) as what the pass writes: turned back into its
* colour, decoded to linear light (the screen image is linear), scaled to the screen's white and premultiplied again.
*/
vec4 overlayOutput(vec4 texel, float whiteLevel) {
    if (texel.a <= 0.0) {
        return vec4(0.0);
    }

    vec3 colour = srgbToLinear(min(texel.rgb / texel.a, vec3(1.0)));
    return vec4(colour * (texel.a * whiteLevel), texel.a);
}

#endif
