#version 450
#extension GL_GOOGLE_include_directive : require

/*
* The depth prepass's fragment shader for cut-outs: as cut_out.frag (only the holes thrown away, and the solid mask
* marked), and a fading level's share of the pixels kept, after triangle.vert, the main pass's own vertex shader
* (cut_out_prepass.glsl, shared with the virtual shadow pages' cut-outs). It takes every output triangle.vert writes,
* though it reads two, since the validation layers warn about a vertex shader's outputs the next stage does not take;
* the shadow pass keeps cut_out.frag, whose vertex shader (shadow_cut_out.vert) writes only those two. An impostor's card
* (scene/impostor.h) finds its holes in its atlas's blended frames, as the main pass reads them.
*/
#include "cut_out_prepass.glsl"

// The solid mask: 0 where a cut-out is nearest (it is drawn after every solid draw, so one in front of it overwrites
// nothing it should not), which contact shadows pass over.
layout(location = 0) out float solid;

void main() {
    if (prepassCutAway()) {
        discard;
    }

    solid = 0.0;
}
