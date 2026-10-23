#version 450
#extension GL_GOOGLE_include_directive : require

/*
* The depth-only passes' fragment shader for cut-outs (the depth prepass and the shadow pass): no colour, only the
* holes thrown away, so their depth is not written there. Opaque draws need no fragment shader at all.
*/
#include "frame_data.glsl"
#include "cut_out_fragment.glsl"

layout(location = 1) in vec2 textureUv;
layout(location = 4) flat in uint materialIndex;

void main() {
    if (fragmentCutAway(materialIndex, textureUv, textureOptions.x)) {
        discard;
    }
}
