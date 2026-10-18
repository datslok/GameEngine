#pragma once

#include <cmath>

/*
* Represents a 2D vector and provides common operations for manipulating vector coordinates, and performing vector calculations needed for geometric operations.
*
* Every function is defined here in the header, not in a .cpp file, so the compiler sees its body wherever it is used and
* can paste it in (inline it) in optimised builds, instead of calling a function to add two numbers.
*/
struct Vec2{
    float x;
    float y;

    // Zero, so a vector always starts with a defined value.
    Vec2() : x(0.0f), y(0.0f) {}
    Vec2(float x, float y) : x(x), y(y) {}

    // Positive when the vectors point in similar directions, negative when they point in opposite ones.
    float dot(const Vec2& other) const { return x * other.x + y * other.y; }

    Vec2 operator+(const Vec2& other) const { return Vec2{x + other.x, y + other.y}; }
    Vec2 operator-(const Vec2& other) const { return Vec2{x - other.x, y - other.y}; }
    Vec2 operator*(float scalar) const { return Vec2{x * scalar, y * scalar}; }
    Vec2 operator/(float scalar) const { return Vec2{x / scalar, y / scalar}; }

    // The length without the square root, for comparing magnitudes cheaply.
    float lengthSquared() const { return dot(*this); }
    float length() const { return std::sqrt(lengthSquared()); }

    // The same direction at unit length; the zero vector stays zero.
    Vec2 normalized() const {
        const float currentLength = length();
        if (currentLength == 0.0f) {
            return Vec2{};
        }
        return *this / currentLength;
    }
};
