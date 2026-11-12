#version 450
#extension GL_GOOGLE_include_directive : require

/*
* The shadow pass's fragment shader for cut-outs: no colour, only the holes thrown away, so their depth is not written
* there. Opaque draws need no fragment shader at all. The depth prepass has its own, cut_out_prepass.frag, taking every
* output of triangle.vert.
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
