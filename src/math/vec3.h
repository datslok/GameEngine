#pragma once

/*
* Represents a 3D vector for storing coordinates and performing vector calculations needed for geometric operations.
*/
struct Vec3{
    float x;
    float y;
    float z;

    Vec3();
    Vec3(float x, float y, float z);

    Vec3 operator+(const Vec3& other) const;
    Vec3 operator-(const Vec3& other) const;

    Vec3 operator*(float scalar) const;
    Vec3 operator/(float scalar) const;

    float dot(const Vec3& other) const;

    float lengthSquared() const;
    float length() const;
    Vec3 normalized() const;
    Vec3 cross(const Vec3& other) const;
};