// Bloom's per-colour arithmetic, included with #include "bloom.glsl": mirrors render/gpu/bloom.cpp, which tests the
// maths, and tests/gpu checks these functions against it on the GPU. The filters themselves read textures and stay in
// bloom_downsample.frag and bloom_upsample.frag.

#ifndef BLOOM_GLSL
#define BLOOM_GLSL

#include "render/gpu/bloom_shared.h"

// The Karis average's weight: 1 / (1 + luminance), so a lone very bright texel counts for little.
float karisWeight(vec3 colour) {
    return 1.0 / (1.0 + dot(colour, vec3(BLOOM_LUMA_RED, BLOOM_LUMA_GREEN, BLOOM_LUMA_BLUE)));
}

// A strength's share of the scene replaced by the blurred scene, so no light is added.
vec3 mixBloom(vec3 scene, vec3 bloom, float strength) {
    return mix(scene, bloom, strength);
}

#endif
