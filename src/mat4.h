#pragma once
#include "vec4.h"
#include "vec3.h"

struct Mat4 {
    float values[4][4];
    Mat4();
    static Mat4 identity();
    Vec4 operator*(const Vec4& vector) const;

    static Mat4 translation(float x, float y, float z);
    static Mat4 scaling(float x, float y, float z);
    static Mat4 rotationX(float radians);
    static Mat4 rotationY(float radians);
    static Mat4 rotationZ(float radians);

    Mat4 operator*(const Mat4& other) const;

    static Mat4 perspective(
        float verticalFovRadians,
        float aspectRatio,
        float nearPlane,
        float farPlane
    );

    static Mat4 lookAt(
    const Vec3& eye,
    const Vec3& target,
    const Vec3& up
    );
};