#version 450
#extension GL_GOOGLE_include_directive : require

/*
* The depth prepass's fragment shader for cut-outs: as cut_out.frag (only the holes thrown away, and the solid mask
* marked), after triangle.vert,
* the main pass's own vertex shader. It takes every output triangle.vert writes, though it reads two, since the
* validation layers warn about a vertex shader's outputs the next stage does not take; the shadow pass keeps
* cut_out.frag, whose vertex shader (shadow_cut_out.vert) writes only those two.
*/
#include "frame_data.glsl"
#include "cut_out_fragment.glsl"

layout(location = 0) in vec3 worldNormal;
layout(location = 1) in vec2 textureUv;
layout(location = 2) in vec3 relativePosition;
layout(location = 3) in vec4 worldTangent;
layout(location = 4) flat in uint materialIndex;
layout(location = 5) flat in float objectScale;
layout(location = 6) flat in vec3 viewerPosition;

// The solid mask: 0 where a cut-out is nearest (it is drawn after every solid draw, so one in front of it overwrites
// nothing it should not), which contact shadows pass over.
layout(location = 0) out float solid;

void main() {
    if (fragmentCutAway(materialIndex, textureUv, textureOptions.x)) {
        discard;
    }

    solid = 0.0;
}
