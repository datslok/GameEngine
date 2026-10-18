#include "render/gpu/gpu_depth_range.h"

/*
* Vulkan (and so SDL_GPU) keeps depths from 0 to 1, while Mat4::perspective follows OpenGL's -1 to 1. Taking half of w
* and subtracting half of depth maps -w..w to w..0, so after the divide by w the near plane is at 1 and the far plane
* at 0 (reversed-Z). x, y and w are untouched.
*
* Why reversed: perspective depth goes as 1/distance, so most distant surfaces crowd into a thin band of depths. A float
* has the same number of values between each power of two, so it is densest near 0 and coarsest near 1. Putting the far
* plane at 1 stacks both effects (the crowded depths land where floats are coarsest, and distant surfaces z-fight);
* putting it at 0 makes them nearly cancel, and depth precision comes out almost even from near to far.
*/
Mat4 toGpuDepthRange(const Mat4& projection) {
    Mat4 correction = Mat4::identity();
    correction.values[2][2] = -0.5f;
    correction.values[2][3] = 0.5f;

    return correction * projection;
}
