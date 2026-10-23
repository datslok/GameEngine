#ifndef RENDER_GPU_TRANSPARENCY_SHARED_H
#define RENDER_GPU_TRANSPARENCY_SHARED_H

/*
* The reactive mask's and glass shadows' numbers (render/gpu/transparency.h), shared by the C++ side and the shaders, so the two cannot
* drift apart. GLSL reads this file too, so it may only hold #defines of plain numbers.
*/

// The mask pass's workgroups are this many pixels on a side (8 x 8 = 64 invocations).
#define TRANSPARENCY_REACTIVE_GROUP_SIDE 8

// The least light a share is measured against, so a black pixel gives 0 rather than 0 / 0: far below anything the
// eye could see, even at night's exposure.
#define TRANSPARENCY_REACTIVE_FLOOR 0.000001

// The most glass objects whose bounds a shadow ray is tested against before it is traced; past it every ray is traced.
#define TRANSPARENCY_GLASS_BOUNDS_LIMIT 64

#endif
