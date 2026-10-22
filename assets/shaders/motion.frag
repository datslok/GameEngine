#version 450
#extension GL_GOOGLE_include_directive : require

// The velocity pass's pixels: the surface's motion since last frame in NDC (render/gpu/taa.h, objectMotion).
#include "taa.glsl"

layout(location = 0) in vec4 currentClip;
layout(location = 1) in vec4 previousClip;

layout(location = 0) out vec2 motion;

void main() {
    motion = objectMotion(currentClip, previousClip);
}
