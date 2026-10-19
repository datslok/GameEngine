#version 450
#extension GL_GOOGLE_include_directive : require

#include "overlay.glsl"

// The overlay (the debug UI): 8-bit, premultiplied, its colours sRGB-encoded as ImGui wrote them. The screen's size.
layout(set = 2, binding = 0) uniform sampler2D overlay;

// Must match OverlayUniformData in render/vulkan/vulkan_overlay.cpp.
layout(std140, set = 3, binding = 0) uniform OverlayData {
    vec4 display; // x: what SDR white is written as (1, or an HDR screen's white level)
};

layout(location = 0) out vec4 outputColour;

/*
* Lays the overlay over the finished screen image (the pipeline blends src + dst * (1 - src alpha)). Mirrored by
* render/gpu/overlay_composite.cpp, which tests the maths.
*/
void main() {
    outputColour = overlayOutput(texelFetch(overlay, ivec2(gl_FragCoord.xy), 0), display.x);
}
