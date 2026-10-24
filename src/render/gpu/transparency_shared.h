#ifndef RENDER_GPU_TRANSPARENCY_SHARED_H
#define RENDER_GPU_TRANSPARENCY_SHARED_H

/*
* The reactive mask's, glass shadows' and refraction's numbers (render/gpu/transparency.h), shared by the C++ side and the shaders, so the two cannot
* drift apart. GLSL reads this file too, so it may only hold #defines of plain numbers.
*/

// The mask pass's workgroups are this many pixels on a side (8 x 8 = 64 invocations).
#define TRANSPARENCY_REACTIVE_GROUP_SIDE 8

// The least light a share is measured against, so a black pixel gives 0 rather than 0 / 0: far below anything the
// eye could see, even at night's exposure.
#define TRANSPARENCY_REACTIVE_FLOOR 0.000001

// The most glass objects whose bounds a shadow ray is tested against before it is traced; past it every ray is traced.
#define TRANSPARENCY_GLASS_BOUNDS_LIMIT 64

// The roughness from which thin glass is frosted, drawn by refraction: its blur (transmissionSpread) can then reach a
// few pixels; smoother glass blurs less than a pixel and blends over the picture instead.
#define TRANSPARENCY_FROSTED_ROUGHNESS 0.05

// The pass that keeps the picture before the glass (opaque_picture.comp): this many pixels on a side per workgroup.
#define TRANSPARENCY_PICTURE_GROUP_SIDE 8

#endif
