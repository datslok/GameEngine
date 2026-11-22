#ifndef SCENE_FOLIAGE_SHARED_H
#define SCENE_FOLIAGE_SHARED_H

// The foliage's shading numbers (scene/foliage_shading.h), shared with the shaders (foliage_shading.glsl). Only #defines
// of plain numbers, since GLSL reads them too.

// A foliage normal: a leaf's own facing at this share, plus its lean (the clump's and crown's ways out, games/forest's
// foliageLean), made unit; never leaning less than this cosine to the facing, so it is never lit from the wrong side.
#define FOLIAGE_FACING_SHARE 0.3
#define FOLIAGE_LEAST_FRONT_SHARE 0.1

// Each leaf's tone (the cards' pictures bake it, the twigs' leaves hash it): a brightness from the darkest over the
// range, and a warmth (red up and blue down by this share, or the reverse), both on the sRGB colour.
#define LEAF_DARKEST_TONE 0.85
#define LEAF_TONE_RANGE 0.25
#define LEAF_MOST_WARMTH 0.06

// A twig leaf's tone seed: one of these many (LEAF_TONE_STEPS brightnesses times as many warmths), carried as its
// texture coordinate's whole part from the vertex shader to the fragment shader.
#define LEAF_TONE_STEPS 8u
#define LEAF_TONE_SEEDS 64u

// pcg3d's third word for leaf tones, so they share no numbers with other hashes of the same twig and leaf.
#define LEAF_TONE_HASH_USE 0x70e5u

// sRGB's gamma, near enough for a tone factor taken to linear light.
#define LEAF_TONE_GAMMA 2.2

#endif
