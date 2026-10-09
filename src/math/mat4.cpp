#include "math/mat4.h"
#include <cmath>
#include <stdexcept>
#include <numbers>

/*
* Initialise the matrix to zero so all components start with defined values.
*/
Mat4::Mat4()
    : values{} {
}

/*
* Create an identity matrix with 1s along the diagonal and 0s elsewhere, which does not change a vector when multiplied.
* Used as a neutral transformation matrix to build other transformations from.
*/
Mat4 Mat4::identity() {
    Mat4 result;

    for (int i = 0; i < 4; ++i){
        result.values[i][i] = 1.0f;
    }

    return result;
}

/*
* Multiply the matrix by a vector to apply the matrix transformation.
*/
Vec4 Mat4::operator*(const Vec4& vector) const{
    return Vec4{
        (values[0][0] * vector.x +
        values[0][1] * vector.y +
        values[0][2] * vector.z +
        values[0][3] * vector.w),

        (values[1][0] * vector.x +
        values[1][1] * vector.y +
        values[1][2] * vector.z +
        values[1][3] * vector.w),

        (values[2][0] * vector.x +
        values[2][1] * vector.y +
        values[2][2] * vector.z +
        values[2][3] * vector.w),

        (values[3][0] * vector.x +
        values[3][1] * vector.y +
        values[3][2] * vector.z +
        values[3][3] * vector.w)
    };
}

/*
* Create a translation matrix to move objects by the specified x,y, and z values.
*/
Mat4 Mat4::translation(float x, float y, float z){
    Mat4 result = identity();

    result.values[0][3] = x;
    result.values[1][3] = y;
    result.values[2][3] = z;

    return result;
}

/*
* Create a scaling matrix to resize objects along the specified x,y, and z axes.
*/ 
Mat4 Mat4::scaling(float x, float y, float z){
    Mat4 result = identity();

    result.values[0][0] = x;
    result.values[1][1] = y;
    result.values[2][2] = z;

    return result;
}

/*
* Create a rotation matrix around the X-axis to rotate objects by the specified angle.
*/
Mat4 Mat4::rotationX(float radians){
    Mat4 result = identity();

    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);

    result.values[1][1] = cosine;
    result.values[1][2] = -sine;
    result.values[2][1] = sine;
    result.values[2][2] = cosine;

    return result;
}

/*
* Create a rotation matrix around the Y-axis to rotate objects by the specified angle.
*/
Mat4 Mat4::rotationY(float radians){
    Mat4 result = identity();

    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);

    result.values[0][0] = cosine;
    result.values[0][2] = sine;
    result.values[2][0] = -sine;
    result.values[2][2] = cosine;

    return result;
}

/*
* Create a rotation matrix around the Z-axis to rotate objects by the specified angle.
*/
Mat4 Mat4::rotationZ(float radians){
    Mat4 result = identity();

    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);

    result.values[0][0] = cosine;
    result.values[0][1] = -sine;
    result.values[1][0] = sine;
    result.values[1][1] = cosine;

    return result;
}

/*
* Multiply two matrices together to combine their transformations into a single matrix.
*/
Mat4 Mat4::operator*(const Mat4& other) const{
    Mat4 result;

    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column){
            for (int index = 0; index < 4; ++index){
                result.values[row][column] += values[row][index] * other.values[index][column];
            }
        }
    }

    return result;
}

/*
* Create a perspective projection matrix to represent depth and field of view.
*/
Mat4 Mat4::perspective(
    float verticalFovRadians,
    float aspectRatio,
    float nearPlane,
    float farPlane
){
    if (!(verticalFovRadians > 0.0f &&
          verticalFovRadians < std::numbers::pi_v<float> &&
          aspectRatio > 0.0f &&
          nearPlane > 0.0f &&
          farPlane > nearPlane)){
        throw std::invalid_argument("Invalid perspective parameters");
    }

    const float focalScale =
        1.0f / std::tan(verticalFovRadians / 2.0f);

    Mat4 result;

    result.values[0][0] = focalScale / aspectRatio;
    result.values[1][1] = focalScale;
    result.values[2][2] = -(farPlane + nearPlane) / (farPlane - nearPlane);
    result.values[2][3] = -(2.0f * farPlane * nearPlane) / (farPlane - nearPlane);
    result.values[3][2] = -1.0f;

    return result;
}

/*
* Create a view matrix from the camera position and orientation to transform world coordinates into camera space.
*/
Mat4 Mat4::lookAt(
    const Vec3& eye,
    const Vec3& target,
    const Vec3& up
){
    const Vec3 offset = target - eye;

    if (offset.lengthSquared() == 0.0f) {
        throw std::invalid_argument(
            "Camera eye and target must be different"
        );
    }

    if (up.lengthSquared() == 0.0f) {
        throw std::invalid_argument(
            "Camera up vector must be nonzero"
        );
    }

    const Vec3 forward = offset.normalized();
    const Vec3 rightCandidate = forward.cross(up.normalized());

    if (rightCandidate.lengthSquared() < 0.000001f) {
        throw std::invalid_argument(
            "Camera up vector must not be parallel to its direction"
        );
    }

    const Vec3 right = rightCandidate.normalized();
    const Vec3 cameraUp = right.cross(forward);

    Mat4 result = identity();

    //The first three columns represent the camera's axes, while the last column accounts for its position. Forward is negated so objects in front of the camera have negative camera-space z.

    result.values[0][0] = right.x;
    result.values[0][1] = right.y;
    result.values[0][2] = right.z;
    result.values[0][3] = -right.dot(eye);

    result.values[1][0] = cameraUp.x;
    result.values[1][1] = cameraUp.y;
    result.values[1][2] = cameraUp.z;
    result.values[1][3] = -cameraUp.dot(eye);

    result.values[2][0] = -forward.x;
    result.values[2][1] = -forward.y;
    result.values[2][2] = -forward.z;
    result.values[2][3] = forward.dot(eye);

    return result;
}

/*
* Normals describe surfaces, and surfaces stretch differently from lines: scale x by 2 and a slope gets shallower, so its normal
* must tip the other way. The inverse transpose does that. It equals the cofactor matrix divided by the determinant.
* Normals are normalised later, so only the determinant's sign matters: a mirrored object (negative determinant) would
* otherwise get inward normals and be lit from behind.
*/
Mat4 normalMatrix(const Mat4& model) {
    const auto& m = model.values;
    Mat4 result;

    // Each cofactor from the 2x2 minor that skips its row and column; taking rows and columns cyclically gives the sign.
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const int row1 = (row + 1) % 3;
            const int row2 = (row + 2) % 3;
            const int column1 = (column + 1) % 3;
            const int column2 = (column + 2) % 3;

            result.values[row][column] = m[row1][column1] * m[row2][column2] - m[row1][column2] * m[row2][column1];
        }
    }

    const float determinant =
        m[0][0] * result.values[0][0] + m[0][1] * result.values[0][1] + m[0][2] * result.values[0][2];

    if (determinant == 0.0f) {
        return Mat4{};
    }

    const float sign = determinant > 0.0f ? 1.0f : -1.0f;

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result.values[row][column] *= sign;
        }
    }

    result.values[3][3] = 1.0f;
    return result;
}

/*
* Scales and shifts the box onto the -1..1 cube, with no division by depth, so things keep their size at any distance.
* Depth follows the same convention as perspective: -1 at the near plane and 1 at the far plane, the camera looking down -z.
*/
Mat4 Mat4::orthographic(float left, float right, float bottom, float top, float nearPlane, float farPlane) {
    // Flipped boxes are allowed (window pixels count down, so their top is 0 and their bottom the height); empty ones are not.
    if (right == left || top == bottom || farPlane == nearPlane) {
        throw std::invalid_argument("Invalid orthographic parameters");
    }

    Mat4 result;

    result.values[0][0] = 2.0f / (right - left);
    result.values[0][3] = -(right + left) / (right - left);
    result.values[1][1] = 2.0f / (top - bottom);
    result.values[1][3] = -(top + bottom) / (top - bottom);
    result.values[2][2] = -2.0f / (farPlane - nearPlane);
    result.values[2][3] = -(farPlane + nearPlane) / (farPlane - nearPlane);
    result.values[3][3] = 1.0f;

    return result;
}