#include "vec2.h"
#include <cmath>

/*
* Initialise the vector to zero so it starts with a defined value.
*/
Vec2::Vec2():
    x(0.0f),
    y(0.0f){
}

/*
* Initialise the vector with the provided x and y coordinates.
*/
Vec2::Vec2(float x, float y):
    x(x),
    y(y){
}

/*
* Calculate the dot product to measure the relationship between the two vectors.
* A positive result indicates the vectors point in a similar direction, while a negative result indicates they point in opposite directions.
*/
float Vec2::dot(const Vec2 &other) const
{
    return x * other.x + y * other.y;
}

/* 
* Add the corresponding components to combine the two vectors.
*/
Vec2 Vec2::operator+(const Vec2 &other) const
{
    return Vec2{x + other.x, y + other.y};
}

/*
* Subtract the corresponding components to find the vector between the two vectors.
*/
Vec2 Vec2::operator-(const Vec2 &other) const
{
    return Vec2(x - other.x, y - other.y);
}

/*
* Scale both components by the given value to resize the vector.
*/
Vec2 Vec2::operator*(float scalar) const
{
    return Vec2(x*scalar, y*scalar);
}

/*
* Divide both components by the given value to resize the vector proportionally.
*/
Vec2 Vec2::operator/(float scalar) const
{
    return Vec2(x/scalar, y/scalar);
}

/*
* Calculate the squared length of the vector to measure the vector's maginitude without the computational cost of a square root.
*/
float Vec2::lengthSquared() const
{
    return dot(*this);
}

float Vec2::length() const
{
    return std::sqrt(lengthSquared());
}

/*
* Scale the vector to unit length so it represents direction without magnitude.
*/
Vec2 Vec2::normalized() const {
    const float currentLength = length();

    if (currentLength == 0.0f) {
        return Vec2{};
    }

    return *this / currentLength;
}

