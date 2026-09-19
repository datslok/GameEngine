#include "vec3.h"
#include <cmath>

/*
* Initialise the vector to zero so it starts with defined coordinates.
*/
Vec3::Vec3():
    x(0.0f),
    y(0.0f),
    z(0.0f){
}

/*
* Initialise the vector with the specified x,y,z coordinates.
*/
Vec3::Vec3(float x, float y, float z):
    x(x),
    y(y),
    z(z){
}

/*
* Add the corresponding components to combine the two vectors.
*/ 
Vec3 Vec3::operator+(const Vec3 &other) const
{
    return Vec3(x + other.x, y + other.y, z + other.z);
}

/*
* Subtract the corresponding components to find the difference between the vectors.
*/
Vec3 Vec3::operator-(const Vec3 &other) const
{
    return Vec3(x - other.x, y - other.y, z - other.z);
}

/*
* Scale each component by the given value to resize the vector.
*/
Vec3 Vec3::operator*(float scalar) const
{
    return Vec3(x*scalar, y*scalar, z*scalar);
}

/*
* Divide each component by the given value to resize the vector proportionally.
*/ 
Vec3 Vec3::operator/(float scalar) const
{
    return Vec3(x/scalar, y/scalar, z/scalar);
}

/*
* Calculate the dot product to measure the relationship between the two vectors.
*/
float Vec3::dot(const Vec3 &other) const
{
    return x * other.x + y * other.y + z * other.z;
}

/*
* Calculate the squared length of the vector to measure the vector's magnitude without the computational cost of a square root, and for more efficient magnitude comparisons.
*/
float Vec3::lengthSquared() const
{
    return dot(*this);
}

/*
*  Calculate the vector's magnitude using its Euclidean length, which is the square root of the sum of the squares of its components.
*/
float Vec3::length() const
{
    return std::sqrt(lengthSquared());
}

/*
* Scale the vector to unit length so it represents direction without magnitude.
*/
Vec3 Vec3::normalized() const
{
    const float currentLength = length();

    if (currentLength == 0.0f) {
        return Vec3{};
    }

    return *this / currentLength;
}


/*
* Calculate a vector perpendicular to both vectors for 3D geometric calculations.
*/
Vec3 Vec3::cross(const Vec3 &other) const
{
    return Vec3(
        y * other.z - z * other.y,
        z * other.x - x * other.z,
        x * other.y - y * other.x
    );
}
