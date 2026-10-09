#include "render/gpu/gpu_depth_range.h"

/*
* Vulkan (and so SDL_GPU) keeps depths from 0 to 1, while Mat4::perspective follows OpenGL's -1 to 1.
* Halving depth and adding half of w maps -w..w to 0..w, so after the divide by w the range is 0..1. x, y and w are untouched.
*/
Mat4 toGpuDepthRange(const Mat4& projection) {
    Mat4 correction = Mat4::identity();
    correction.values[2][2] = 0.5f;
    correction.values[2][3] = 0.5f;

    return correction * projection;
}
