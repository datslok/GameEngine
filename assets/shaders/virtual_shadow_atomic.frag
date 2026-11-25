#version 450
#extension GL_GOOGLE_include_directive : require

/*
* The sun's virtual shadow pages drawn by atomics (virtual_shadow_draw.mesh; ROADMAP phase 21 4c-iv): no depth target,
* each fragment's reversed depth kept by imageAtomicMax in the pool's whole-number view at its own texel, the nearest
* surface winning whatever the order, as the depth test GREATER keeps it. Cut-outs (the constant on) throw their holes
* away first, as the depth prepass does (cut_out_prepass.glsl).
*/
#include "cut_out_prepass.glsl"
#include "virtual_shadows.glsl"

layout(constant_id = 0) const bool cutOut = false;

layout(set = 1, binding = SCENE_VIRTUAL_SHADOW_ATOMIC_POOL_BINDING, r32ui) uniform uimage2D virtualShadowAtomicPool;

void main() {
    if (cutOut && prepassCutAway()) {
        discard;
    }

    imageAtomicMax(virtualShadowAtomicPool, ivec2(gl_FragCoord.xy), virtualShadowAtomicDepth(gl_FragCoord.z));
}
