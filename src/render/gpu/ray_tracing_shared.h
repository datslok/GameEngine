#ifndef RENDER_GPU_RAY_TRACING_SHARED_H
#define RENDER_GPU_RAY_TRACING_SHARED_H

// Hardware ray tracing's numbers, shared with the shaders (ray_tracing.glsl, ray_scene.glsl, ray_debug.frag). Only
// #defines of plain numbers, since GLSL includes this file too; C++ reads them as typed constants in render/gpu/ray_tracing.h.

// Each instance carries a mask; a ray sees an instance when its own mask shares a bit with it. Solid things block light
// (shadow casters); visual-only things are seen but stand in for a light that is counted already (the sun disc, the
// moon, lamp bulbs), so light-gathering rays leave them out.
#define RAY_MASK_SOLID 1
#define RAY_MASK_VISUAL_ONLY 2
#define RAY_MASK_EVERYTHING 255

// A mesh's vertices as floats (MeshVertex: position, normal, uv, tangent), read through the vertex buffer's address.
#define MESH_VERTEX_FLOATS 12
#define MESH_VERTEX_POSITION 0
#define MESH_VERTEX_NORMAL 3
#define MESH_VERTEX_UV 6

// What the ray tracing debug view shows (RayDebugView).
#define RAY_DEBUG_OFF 0
#define RAY_DEBUG_NORMALS 1
#define RAY_DEBUG_ALBEDO 2
#define RAY_DEBUG_FACES 3

#endif
