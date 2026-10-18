#pragma once
#include "math/vec4.h"
#include "math/vec3.h"

#include <optional>

/*
* Represents a 4x4 matrix for storing and combining transformations used in 3D graphics and coordinate calculations.
*/
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

    // A box-shaped view with no perspective (parallel rays): the box from left..right, bottom..top, and -nearPlane..-farPlane
    // along z maps to the -1..1 cube, with the same depth convention as perspective. Swapping bottom and top flips y.
    static Mat4 orthographic(float left, float right, float bottom, float top, float nearPlane, float farPlane);

    static Mat4 lookAt(
    const Vec3& eye,
    const Vec3& target,
    const Vec3& up
    );
};

// The matrix that transforms normals for this model matrix: the inverse transpose of its 3x3 part, in the top-left with [3][3] = 1.
// Zero when the model matrix squashes space flat.
Mat4 normalMatrix(const Mat4& model);

// The inverse of a model matrix (any translation, rotation and scale; the last row must be 0 0 0 1), or nothing when it
// squashes space flat. Used to carry a world ray into an object's own coordinates.
std::optional<Mat4> affineInverse(const Mat4& model);

// A position moved by the matrix (w = 1, so translation applies) and a direction (w = 0, so it does not).
Vec3 transformPoint(const Mat4& matrix, const Vec3& point);
Vec3 transformDirection(const Mat4& matrix, const Vec3& direction);
