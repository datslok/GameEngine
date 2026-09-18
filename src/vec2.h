#pragma once

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