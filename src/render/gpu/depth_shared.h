#ifndef RENDER_GPU_DEPTH_SHARED_H
#define RENDER_GPU_DEPTH_SHARED_H

// Reversed-Z: the depths the GPU stores for the near and far planes, shared with the shaders. Only #defines of plain
// numbers, since GLSL includes this file too; C++ reads them as gpuNearDepth and gpuFarDepth (render/gpu/gpu_depth_range.h).
#define GPU_NEAR_DEPTH 1.0
#define GPU_FAR_DEPTH 0.0

#endif
