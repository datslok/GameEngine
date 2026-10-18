#pragma once

#include "math/mat4.h"
#include "render/gpu/depth_shared.h"

// Reversed-Z: the GPU stores 1 at the near plane and 0 at the far plane, so nearer surfaces have larger depths. The
// depth tests keep the greater value, depth images are cleared to the far depth, 0, and shadow lookups compare the
// other way round too. Shared with the shaders (depth_shared.h).
inline constexpr float gpuNearDepth = static_cast<float>(GPU_NEAR_DEPTH);
inline constexpr float gpuFarDepth = static_cast<float>(GPU_FAR_DEPTH);

// Turn a projection that gives OpenGL depth (-1 at the near plane, 1 at the far plane) into one that gives the GPU's
// reversed 1..0. Works for perspective and orthographic projections alike.
Mat4 toGpuDepthRange(const Mat4& projection);
