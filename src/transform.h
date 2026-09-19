#pragma once

#include "mat4.h"
#include "vec3.h"

struct Transform {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 rotation{0.0f, 0.0f, 0.0f}; // Radians
    Vec3 scale{1.0f, 1.0f, 1.0f};

    Mat4 getMatrix() const;
};