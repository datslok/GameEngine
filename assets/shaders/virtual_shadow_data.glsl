#ifndef VIRTUAL_SHADOW_DATA_GLSL
#define VIRTUAL_SHADOW_DATA_GLSL

// The clipmap's record (render/gpu/virtual_shadows.h, GpuVirtualShadowFrame, std430), declared by each reader in its own
// set: the marking and resolving passes at set 0, the main pass at set 1 binding SCENE_VIRTUAL_SHADOW_FRAME_BINDING.

#include "render/gpu/virtual_shadows_shared.h"

struct VirtualShadowLevelData {
    vec4 placement; // xy the camera's texel, z the inverse texel size, w the depth scale
    vec4 depth;     // x the depth offset
    ivec4 pages;    // xy the first page's table slot, zw the first page across the light (low 32 bits)
    uvec4 key;      // x the level's generation
};

#define VIRTUAL_SHADOW_FRAME_FIELDS                                                                                     \
    mat4 lightRotation;          /* camera-relative points to the light's axes */                                  \
    mat4 inverseViewProjection;  /* the camera's, camera-relative and jittered */                                  \
    vec4 screen;                 /* the render size in pixels, and its inverse */                                  \
    vec4 options;                /* x the first usable half-width, y 1 the debug view, z 1 starts again, w 1 drawn */\
    ivec4 counts;                /* x the levels, y the pages a level's side, z the pool's pages, w the frame */  \
    ivec4 pool;                  /* x the pool's pages a row, y the pairs' room, z, w its width, height (texels) */\
    VirtualShadowLevelData levels[VIRTUAL_SHADOW_MAX_LEVELS];

#endif
