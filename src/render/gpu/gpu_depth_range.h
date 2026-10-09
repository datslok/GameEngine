#pragma once

#include "math/mat4.h"

// Turn a projection that gives OpenGL depth (-1 at the near plane, 1 at the far plane) into one that gives the GPU's 0..1.
Mat4 toGpuDepthRange(const Mat4& projection);
