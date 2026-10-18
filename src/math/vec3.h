#pragma once

#include <cmath>

/*
* Represents a 3D vector for storing coordinates and performing vector calculations needed for geometric operations.
*
* Every function is defined here in the header, not in a .cpp file, so the compiler sees its body wherever it is used and
* can paste it in (inline it) in optimised builds. Physics does thousands of these small operations per tick, and calling
* a function for each one cost several times more than the arithmetic itself.
*/
struct Vec3{
    float x;
    float y;
    float z;

    // Zero, so a vector always starts with defined coordinates.
    Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vec3 operator+(const Vec3& other) const { return Vec3{x + other.x, y + other.y, z + other.z}; }
    Vec3 operator-(const Vec3& other) const { return Vec3{x - other.x, y - other.y, z - other.z}; }

    Vec3 operator*(float scalar) const { return Vec3{x * scalar, y * scalar, z * scalar}; }
    Vec3 operator/(float scalar) const { return Vec3{x / scalar, y / scalar, z / scalar}; }

    // Component by component, as GLSL multiplies vectors: a colour tinting light (red times red, green times green).
    Vec3 operator*(const Vec3& other) const { return Vec3{x * other.x, y * other.y, z * other.z}; }

    float dot(const Vec3& other) const { return x * other.x + y * other.y + z * other.z; }

    // The length without the square root, for comparing magnitudes cheaply.
    float lengthSquared() const { return dot(*this); }
    float length() const { return std::sqrt(lengthSquared()); }

    // The same direction at unit length; the zero vector stays zero.
    Vec3 normalized() const {
        const float currentLength = length();
        if (currentLength == 0.0f) {
            return Vec3{};
        }
        return *this / currentLength;
    }

    // Perpendicular to both vectors (right-handed), with length |a| |b| sin(angle).
    Vec3 cross(const Vec3& other) const {
        return Vec3{
            y * other.z - z * other.y,
            z * other.x - x * other.z,
            x * other.y - y * other.x
        };
    }

    // x, y or z for index 0, 1 or 2, so code can loop over the axes.
    float operator[](int index) const { return index == 0 ? x : (index == 1 ? y : z); }
};
