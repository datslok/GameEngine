// Hashes for noise, included with #include "hash.glsl": mirrors core/hash.h, bit for bit.

#ifndef HASH_GLSL
#define HASH_GLSL

// PCG3D (Jarzynski and Olano 2020), core/hash.h's pcg3d: three words in, three well-mixed words out, the same bits.
uvec3 pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

#endif
