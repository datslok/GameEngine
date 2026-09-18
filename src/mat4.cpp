#include "mat4.h"
#include <cmath>
#include <stdexcept>
#include <numbers>

Mat4::Mat4()
    : values{} {
}

Mat4 Mat4::identity() {
    Mat4 result;

    for (int i = 0; i < 4; ++i){
        result.values[i][i] = 1.0f;
    }

    return result;
}

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

Mat4 Mat4::translation(float x, float y, float z){
    Mat4 result = identity();

    result.values[0][3] = x;
    result.values[1][3] = y;
    result.values[2][3] = z;

    return result;
}

Mat4 Mat4::scaling(float x, float y, float z){
    Mat4 result = identity();

    result.values[0][0] = x;
    result.values[1][1] = y;
    result.values[2][2] = z;

    return result;
}

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

    //The first three columns express the cameras axes. The last column accounts for its position. We use negative forward because objects in front of our camera must have negative camera-space Z.

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
