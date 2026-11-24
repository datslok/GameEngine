#version 450
#extension GL_GOOGLE_include_directive : require

/*
* The sun's virtual shadow pages' cut-outs (virtual_shadow_draw.mesh): no colour, the holes thrown away as the depth
* prepass throws them (cut_out_prepass.glsl), so a leaf casts the shape it is seen with.
*/
#include "cut_out_prepass.glsl"

void main() {
    if (prepassCutAway()) {
        discard;
    }
}
