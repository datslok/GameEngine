#include "mat4.h"
#include <cmath>

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
