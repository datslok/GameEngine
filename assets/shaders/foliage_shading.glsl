// How foliage is shaded (ROADMAP phase 21 stage 3b-iii-c): the normals that light a crown as clumps, and each leaf's own
// tone. Mirrors scene/foliage_shading.cpp, which tests the maths; tests/gpu checks these functions against it.

#ifndef FOLIAGE_SHADING_GLSL
#define FOLIAGE_SHADING_GLSL

#include "hash.glsl"
#include "scene/foliage_shared.h"

// A leaf's facing bent by its lean: the facing times its share plus the lean, made unit, never leaning less than
// FOLIAGE_LEAST_FRONT_SHARE to the facing.
vec3 bentFoliageNormal(vec3 facing, vec3 lean) {
    vec3 blend = normalize(facing * float(FOLIAGE_FACING_SHARE) + lean);
    float front = dot(blend, facing);
    float least = float(FOLIAGE_LEAST_FRONT_SHARE);
    return front >= least ? blend : normalize(blend + facing * (least - front));
}

// A modelled leaf's unit normal between its own (blend 0) and bent by its twig's lean (1), made unit.
vec3 blendedFoliageNormal(vec3 normal, vec3 lean, float blend) {
    vec3 bent = bentFoliageNormal(normal, lean);
    return normalize(normal * (1.0 - blend) + bent * blend);
}

// A twig leaf's tone seed from its plant (its pick's bits), its twig's number in its look and its leaf's in the twig.
uint leafToneSeed(uint plant, uint twig, uint leaf) {
    return pcg3d(uvec3(plant, twig, leaf ^ (LEAF_TONE_HASH_USE << 16u))).x % LEAF_TONE_SEEDS;
}

float leafTone(uint seed) {
    return float(LEAF_DARKEST_TONE) + float(LEAF_TONE_RANGE) * float(seed % LEAF_TONE_STEPS) / float(LEAF_TONE_STEPS - 1u);
}

float leafWarmth(uint seed) {
    return float(LEAF_MOST_WARMTH) * (2.0 * float(seed / LEAF_TONE_STEPS % LEAF_TONE_STEPS) / float(LEAF_TONE_STEPS - 1u) - 1.0);
}

// What a leaf's linear colour is multiplied by: its tone and warmth on the sRGB colour, taken to linear light.
vec3 leafToneScale(uint seed) {
    float tone = leafTone(seed);
    float warmth = leafWarmth(seed);
    return pow(vec3(tone * (1.0 + warmth), tone, tone * (1.0 - warmth)), vec3(float(LEAF_TONE_GAMMA)));
}

// The vertex shader swaps a prototype's leaf number (the texture coordinate's whole part) for its tone seed with its
// twig; the fragment shader reads the seed back. The fraction, which the repeating texture reads, is kept.
float tonedLeafU(float numberedU, uint plant, uint twig) {
    float leaf = floor(numberedU);
    return numberedU - leaf + float(leafToneSeed(plant, twig, uint(leaf)));
}

uint leafToneSeedOf(float u) {
    return uint(max(floor(u), 0.0)) % LEAF_TONE_SEEDS;
}

#endif
