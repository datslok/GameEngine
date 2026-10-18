#include "math/aabb.h"

#include <numeric>

/*
* The point halfway between the corners. std::midpoint avoids overflow for huge coordinates.
*/
Vec3 Aabb::centre() const {
    return Vec3{
        std::midpoint(minimum.x, maximum.x),
        std::midpoint(minimum.y, maximum.y),
        std::midpoint(minimum.z, maximum.z)
    };
}

/*
* The box's extent along each axis.
*/
Vec3 Aabb::size() const {
    return maximum - minimum;
}
