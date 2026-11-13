// The record and focus the depth of field passes reading the depth share (depth_of_field_focus.comp,
// depth_of_field_prepare.comp and depth_of_field_composite.comp), at set 0 bindings 0 and 1, and the blur radius of a
// render texel from them. Included after depth_of_field.glsl and screen_reflections.glsl.

#ifndef DEPTH_OF_FIELD_PASS_GLSL
#define DEPTH_OF_FIELD_PASS_GLSL

#include "render/gpu/depth_shared.h"

// Must match GpuDepthOfFieldPass.
layout(std430, set = 0, binding = 0) readonly buffer Pass {
    mat4 inverseViewProjection; // camera-relative, as the frame was drawn (jitter included)
    vec4 renderSize;            // xy: the depth image's size, zw: its inverse
    vec4 outputSize;            // xy: the picture's size, zw: its inverse
    vec4 lens;                  // x: the focal length (m), y: the lens scale, z: the nearest focus's inverse, w: 1 to snap
    vec4 focus;                 // x: the set focus's inverse, y: 1 for autofocus, z: seconds since last frame, w: the time constant
    vec4 options;               // x: 1 to tint the blur
} pass;

// The eased focus, kept from frame to frame: x its inverse distance (dioptres).
layout(std430, set = 0, binding = 1) buffer Focus {
    vec4 inverseFocus;
} focusState;

// The inverse view distance at a render texel of this depth (0 for the sky, at infinity).
float inverseDistanceAt(ivec2 renderTexel, float depth) {
    if (depth == float(GPU_FAR_DEPTH)) {
        return 0.0;
    }

    return 1.0 / viewDistanceAtPixel(pass.inverseViewProjection, vec2(renderTexel) + 0.5, depth, pass.renderSize.xy);
}

// The render texel under an output pixel (the depth may be smaller, while upscaling).
ivec2 renderTexelUnder(ivec2 pixel) {
    return clamp(ivec2((vec2(pixel) + 0.5) * pass.renderSize.xy * pass.outputSize.zw), ivec2(0), ivec2(pass.renderSize.xy) - 1);
}

// The blur radius (blur pixels) at an output pixel, from the depth under it and this frame's focus.
float outputBlurRadius(sampler2D depthImage, ivec2 pixel) {
    ivec2 renderTexel = renderTexelUnder(pixel);
    float inverseDistance = inverseDistanceAt(renderTexel, texelFetch(depthImage, renderTexel, 0).r);
    return blurRadius(inverseDistance, focusState.inverseFocus.x, pass.lens.x, pass.lens.y);
}

#endif
