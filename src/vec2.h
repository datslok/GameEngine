#pragma once

/*
* Represents a 2D vector and provides common operations for manipulating vector coordinates, and performing vector calculations needed for geometric operations.
*/
struct Vec2{
    float x;
    float y;

    Vec2();
    Vec2(float x, float y);

    float dot(const Vec2& other) const;

    Vec2 operator+(const Vec2& other) const;
    Vec2 operator-(const Vec2& other) const;
    Vec2 operator*(float scalar) const;
    Vec2 operator/(float scalar) const;

    float lengthSquared() const;
    float length() const;
    Vec2 normalized() const;
};