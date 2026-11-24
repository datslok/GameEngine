// The depth prepass's cut-out test (cut_out_prepass.frag), shared by the sun's virtual shadow pages' cut-outs
// (virtual_shadow_cut_out.frag): every output of the main pass's vertex body taken (the validation layers warn about
// outputs the next stage does not take), the holes thrown away, an impostor's card's found in its atlas's blended frames
// as the main pass reads them, and a fading level's share of the pixels kept.
#ifndef CUT_OUT_PREPASS_GLSL
#define CUT_OUT_PREPASS_GLSL

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

// Whether the fragment is thrown away: in a hole, or outside its fading level's share.
bool prepassCutAway() {
    // The card point's steps, taken before any branch, as derivatives must be.
    vec3 stepAcross = dFdx(framePosition);
    vec3 stepDown = dFdy(framePosition);
    MaterialData material = materials[materialIndex];

    if ((material.extra.y & MATERIAL_IMPOSTOR) != 0u) {
        ImpostorSample tree = sampleImpostor(material.textureSlots.x, material.textureSlots.z, material.attenuation.w, framePosition, worldTangent.xyz,
                                             stepAcross, stepDown, textureOptions.x);

        if (isCutAway(tree.colour.a * material.baseColour.a, material.emissive.w)) {
            return true;
        }
    } else if (fragmentCutAway(materialIndex, textureUv, textureOptions.x)) {
        return true;
    }

    // A level fading (level_fade.glsl) keeps its share of the pixels, the main pass then shading only those (EQUAL); tested
    // last, so the texture reads above keep their neighbours for their derivatives.
    return !levelFadeKeeps(levelFade, levelFadeNoise(gl_FragCoord.xy, uint(levelOptions.y)));
}

#endif
