#version 450
#extension GL_GOOGLE_include_directive : require

/*
* The depth prepass's fragment shader for cut-outs: as cut_out.frag (only the holes thrown away, and the solid mask
* marked), and a fading level's share of the pixels kept, after triangle.vert,
* the main pass's own vertex shader. It takes every output triangle.vert writes, though it reads two, since the
* validation layers warn about a vertex shader's outputs the next stage does not take; the shadow pass keeps
* cut_out.frag, whose vertex shader (shadow_cut_out.vert) writes only those two. An impostor's card (scene/impostor.h)
* finds its holes in its atlas's blended frames, as the main pass reads them.
*/
#include "render/gpu/material_shared.h"
#include "frame_data.glsl"
#include "cut_out_fragment.glsl"
#include "impostor_sampling.glsl"
#include "level_fade.glsl"

layout(location = 0) in vec3 worldNormal;
layout(location = 1) in vec2 textureUv;
layout(location = 2) in vec3 relativePosition;
layout(location = 3) in vec4 worldTangent;
layout(location = 4) flat in uint materialIndex;
layout(location = 5) flat in float objectScale;
layout(location = 6) flat in vec3 viewerPosition;
layout(location = 7) in vec3 framePosition;
layout(location = 11) flat in uint levelFade;

// The solid mask: 0 where a cut-out is nearest (it is drawn after every solid draw, so one in front of it overwrites
// nothing it should not), which contact shadows pass over.
layout(location = 0) out float solid;

void main() {
    // The card point's steps, taken before any branch, as derivatives must be.
    vec3 stepAcross = dFdx(framePosition);
    vec3 stepDown = dFdy(framePosition);
    MaterialData material = materials[materialIndex];

    if ((material.extra.y & MATERIAL_IMPOSTOR) != 0u) {
        ImpostorSample tree = sampleImpostor(material.textureSlots.x, material.textureSlots.z, material.attenuation.w, framePosition, worldTangent.xyz,
                                             stepAcross, stepDown, textureOptions.x);

        if (isCutAway(tree.colour.a * material.baseColour.a, material.emissive.w)) {
            discard;
        }
    } else if (fragmentCutAway(materialIndex, textureUv, textureOptions.x)) {
        discard;
    }

    // A level fading (level_fade.glsl) keeps its share of the pixels, the main pass then shading only those (EQUAL); tested
    // last, so the texture reads above keep their neighbours for their derivatives.
    if (!levelFadeKeeps(levelFade, levelFadeNoise(gl_FragCoord.xy, uint(levelOptions.y)))) {
        discard;
    }

    solid = 0.0;
}
