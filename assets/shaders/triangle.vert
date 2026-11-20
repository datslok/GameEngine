#version 450
#extension GL_GOOGLE_include_directive : require
// gl_BaseVertexARB: the drawn mesh's first vertex in the shared mesh buffer (render/gpu/mesh_pool.h); the per-vertex
// records (hanging things, thinned cards) count from the mesh's own first vertex.
#extension GL_ARB_shader_draw_parameters : require

#include "triangle_vertex.glsl"
