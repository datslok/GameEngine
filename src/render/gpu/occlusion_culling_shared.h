#ifndef RENDER_GPU_OCCLUSION_CULLING_SHARED_H
#define RENDER_GPU_OCCLUSION_CULLING_SHARED_H

// Occlusion culling's numbers, shared by render/gpu/occlusion_culling.h and the shaders (occlusion_culling.glsl,
// occlusion_count.comp). Only #defines of plain numbers, since GLSL reads them too.

// The counting pass's workgroup: one batch at a time, its instances shared out among this many invocations.
#define OCCLUSION_GROUP_SIZE 64

// The most workgroups the counting pass dispatches; each takes every this-many-th batch (Vulkan guarantees 65535).
#define OCCLUSION_MAX_GROUPS 65535

// Pixels added round each sphere's rectangle: the depth was drawn with TAA's jitter (under half a pixel) and the
// rectangle's edges are rounded, so this keeps the test from reading past the sphere's real edge.
#define OCCLUSION_RECT_MARGIN 1

#endif
