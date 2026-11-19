#ifndef CORE_HASH_SHARED_H
#define CORE_HASH_SHARED_H

// The hashes' numbers (core/hash.h and its shader copy, hash64.glsl). Only #defines of plain numbers, since GLSL
// includes this file too (as 64-bit literals, with GL_EXT_shader_explicit_arithmetic_types_int64).

// SplitMix64's finaliser's two multipliers (mixBits).
#define HASH_MIX_FIRST 0xbf58476d1ce4e5b9UL
#define HASH_MIX_SECOND 0x94d049bb133111ebUL

// The golden ratio's fraction in 64-bit fixed point: added before mixing so neighbouring numbers land far apart.
#define HASH_GOLDEN_GAMMA 0x9e3779b97f4a7c15UL

#endif
