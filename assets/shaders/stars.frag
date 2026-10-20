#version 460
#extension GL_GOOGLE_include_directive : require

// A star's splat (stars.vert): each pixel its tent-weighted share of the star's light, so the shares sum to all of it
// wherever the star falls (render/gpu/stars.h, starSplatWeight).
#include "stars.glsl"

layout(location = 0) flat in vec3 radiance;
layout(location = 1) flat in vec2 starPixel;

layout(location = 0) out vec4 colour;

void main() {
    colour = vec4(radiance * starSplatWeight(gl_FragCoord.xy - starPixel), 1.0);
}
