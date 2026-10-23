#version 450
#extension GL_GOOGLE_include_directive : require

// The velocity pass's pixels: the surface's motion since last frame in NDC (render/gpu/taa.h, objectMotion), a cut-out's
// holes thrown away in its own pipeline (a specialization constant), so what moves behind them keeps its own motion.
#include "taa.glsl"
#include "frame_data.glsl"
#include "cut_out_fragment.glsl"
#include "render/gpu/material_shared.h"

layout(constant_id = MATERIAL_CUT_OUT_CONSTANT) const bool cutOut = false;

layout(location = 0) in vec4 currentClip;
layout(location = 1) in vec4 previousClip;
layout(location = 2) in vec2 textureUv;
layout(location = 3) flat in uint materialIndex;

layout(location = 0) out vec2 motion;

void main() {
    if (cutOut && fragmentCutAway(materialIndex, textureUv, textureOptions.x)) {
        discard;
    }

    motion = objectMotion(currentClip, previousClip);
}
