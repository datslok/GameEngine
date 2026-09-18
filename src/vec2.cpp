#include "vec2.h"
#include <cmath>

Vec2::Vec2():
    x(0.0f),
    y(0.0f){
}

Vec2::Vec2(float x, float y):
    x(x),
    y(y){
}

float Vec2::dot(const Vec2 &other) const
{
    return x * other.x + y * other.y;
}

Vec2 Vec2::operator+(const Vec2 &other) const
{
    return Vec2{x + other.x, y + other.y};
}

Vec2 Vec2::operator-(const Vec2 &other) const
{
    return Vec2(x - other.x, y - other.y);
}

Vec2 Vec2::operator*(float scalar) const
{
    return Vec2(x*scalar, y*scalar);
}

Vec2 Vec2::operator/(float scalar) const
{
    return Vec2(x/scalar, y/scalar);
}

float Vec2::lengthSquared() const
{
    return dot(*this);
}

float Vec2::length() const
{
    return std::sqrt(lengthSquared());
}

Vec2 Vec2::normalized() const {
    const float currentLength = length();

    if (currentLength == 0.0f) {
        return Vec2{};
    }

    return *this / currentLength;
}

