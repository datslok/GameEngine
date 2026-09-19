#include "vec4.h"

/*
* Initialise the vector to zero so all components start with defined values.
*/
Vec4::Vec4():
    x(0.0f), 
    y(0.0f), 
    z(0.0f), 
    w(0.0f){
}

/*
* Initialise the vector with the specified x,y,z,w coordinates.
*/
Vec4::Vec4(float x, float y, float z, float w):
    x(x), 
    y(y), 
    z(z), 
    w(w){
}

/*
* Add the corresponding components to combine the two vectors.
*/
Vec4 Vec4::operator+(const Vec4& other) const{
    return Vec4{
        x + other.x,
        y + other.y,
        z + other.z,
        w + other.w
    };
}

/*
* Subtract the corresponding components to find the difference between the vectors.
*/
Vec4 Vec4::operator-(const Vec4& other) const{
    return Vec4{
        x - other.x,
        y - other.y,
        z - other.z,
        w - other.w
    };
}

/*
* Scale each component by the given value to change the vector's magnitude.
*/
Vec4 Vec4::operator*(float scalar) const{
    return Vec4{
        x * scalar,
        y * scalar,
        z * scalar,
        w * scalar
    };
}