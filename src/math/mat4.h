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
    // The matrix applied to a column vector. Defined in the header (below) so optimised builds can inline it.
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

// -1 when the model matrix mirrors space (its 3x3 part has a negative determinant), else +1. Normal maps need it: a
// mirror reverses which side of the tangent the bitangent is on.
float handedness(const Mat4& model);

// The inverse of a model matrix (any translation, rotation and scale; the last row must be 0 0 0 1), or nothing when it
// squashes space flat. Used to carry a world ray into an object's own coordinates.
std::optional<Mat4> affineInverse(const Mat4& model);

inline Vec4 Mat4::operator*(const Vec4& vector) const {
    return Vec4{
        values[0][0] * vector.x + values[0][1] * vector.y + values[0][2] * vector.z + values[0][3] * vector.w,
        values[1][0] * vector.x + values[1][1] * vector.y + values[1][2] * vector.z + values[1][3] * vector.w,
        values[2][0] * vector.x + values[2][1] * vector.y + values[2][2] * vector.z + values[2][3] * vector.w,
        values[3][0] * vector.x + values[3][1] * vector.y + values[3][2] * vector.z + values[3][3] * vector.w
    };
}

// A position moved by the matrix (w = 1, so translation applies) and a direction (w = 0, so it does not).
// An arrow does not change when the thing it belongs to moves, only when it turns or stretches. Inline, like the operator above.
inline Vec3 transformPoint(const Mat4& matrix, const Vec3& point) {
    const Vec4 moved = matrix * Vec4{point.x, point.y, point.z, 1.0f};
    return Vec3{moved.x, moved.y, moved.z};
}

inline Vec3 transformDirection(const Mat4& matrix, const Vec3& direction) {
    const Vec4 turned = matrix * Vec4{direction.x, direction.y, direction.z, 0.0f};
    return Vec3{turned.x, turned.y, turned.z};
}
