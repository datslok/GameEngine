// The 64-bit hash, included with #include "hash64.glsl" by a shader that turns on
// GL_EXT_shader_explicit_arithmetic_types_int64 (hash.glsl's are 32-bit, for every shader): mirrors core/hash.h, bit for bit.

#ifndef HASH64_GLSL
#define HASH64_GLSL

#include "core/hash_shared.h"

// SplitMix64's finaliser, core/hash.h's mixBits.
uint64_t mixBits(uint64_t value) {
    value = (value ^ (value >> 30)) * HASH_MIX_FIRST;
    value = (value ^ (value >> 27)) * HASH_MIX_SECOND;
    return value ^ (value >> 31);
}

#endif
