// Throwing away a cut-out's holes (render/gpu/alpha_mask.h), for the passes that only need to know where a surface is:
// the depth prepass, the shadow pass and the velocity pass. The main pass tests the colour it has already read.
#ifndef CUT_OUT_FRAGMENT_GLSL
#define CUT_OUT_FRAGMENT_GLSL

#include "materials.glsl"
#include "alpha_mask.glsl"

// Whether the fragment of this material at uv lies in a hole: its colour texture's alpha, read with mipBias, times the
// material's opacity, under its cutoff.
bool fragmentCutAway(uint materialIndex, vec2 uv, float mipBias) {
    MaterialData material = materials[materialIndex];
    float alpha = texture(textures[nonuniformEXT(material.textureSlots.x)], uv, mipBias).a * material.baseColour.a;
    return isCutAway(alpha, material.emissive.w);
}

#endif
