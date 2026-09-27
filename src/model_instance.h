#pragma once

#include "mat4.h"
#include "transform.h"
#include "vec3.h"

struct ModelInstance {
    Transform transform;

    Mat4 normalization = Mat4::identity();

    Vec3 initialRotation{0.0f, 0.0f, 0.0f};
    Vec3 rotationSpeed{0.0f, 0.0f, 0.0f};

    Mat4 getMatrix() const;
};