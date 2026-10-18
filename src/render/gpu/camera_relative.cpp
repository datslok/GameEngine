#include "render/gpu/camera_relative.h"
#include "render/gpu/gpu_depth_range.h"

/*
* The same lens and the same turn as the camera's own view, from a copy of the camera moved to the origin: its view
* matrix then has no translation left in it.
*/
Mat4 cameraRelativeViewProjection(const Camera& camera) {
    Camera atOrigin = camera;
    atOrigin.setPosition(Vec3{0.0f, 0.0f, 0.0f});

    return toGpuDepthRange(atOrigin.getProjectionMatrix()) * atOrigin.getViewMatrix();
}

/*
* Moving by -origin first: translation(-origin) * model. Written out, since for a model matrix (last row 0 0 0 1) it
* subtracts the origin from the translation and leaves everything else exactly as it was.
*/
Mat4 relativeTo(const Mat4& model, const Vec3& origin) {
    Mat4 moved = model;
    const float shift[3] = {origin.x, origin.y, origin.z};

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            moved.values[row][column] -= shift[row] * model.values[3][column];
        }
    }

    return moved;
}
