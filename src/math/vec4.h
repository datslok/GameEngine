#pragma once

/*
* Represents a 4D vector for storing coordinates and performing vector calculations needed for geometric operations.
*
* Defined in the header so optimised builds can inline every operation (see Vec3).
*/
struct Vec4 {
    float x;
    float y;
    float z;
    float w;

    // Zero, so all components start with defined values.
    Vec4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
    Vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

    Vec4 operator+(const Vec4& other) const { return Vec4{x + other.x, y + other.y, z + other.z, w + other.w}; }
    Vec4 operator-(const Vec4& other) const { return Vec4{x - other.x, y - other.y, z - other.z, w - other.w}; }
    Vec4 operator*(float scalar) const { return Vec4{x * scalar, y * scalar, z * scalar, w * scalar}; }
};
