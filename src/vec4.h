#pragma once

/*
* Represents a 4D vector for storing coordinates and performing vector calculations needed for geometric operations.
*/
struct Vec4 {
    float x;
    float y;
    float z;
    float w;

    Vec4();
    Vec4(float x, float y, float z, float w);

    Vec4 operator+(const Vec4& other) const;
    Vec4 operator-(const Vec4& other) const;
    Vec4 operator*(float scalar) const;
};