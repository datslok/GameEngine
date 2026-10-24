#include "math/aabb.h"

#include <algorithm>
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

/*
* Each axis separately: between the corners on all three.
*/
bool Aabb::contains(const Vec3& point) const {
    return point.x >= minimum.x && point.x <= maximum.x && point.y >= minimum.y && point.y <= maximum.y && point.z >= minimum.z &&
           point.z <= maximum.z;
}

/*
* Each axis separately: the lower of the two minimums and the higher of the two maximums.
*/
Aabb enclosing(const Aabb& first, const Aabb& second) {
    return Aabb{
        Vec3{std::min(first.minimum.x, second.minimum.x), std::min(first.minimum.y, second.minimum.y), std::min(first.minimum.z, second.minimum.z)},
        Vec3{std::max(first.maximum.x, second.maximum.x), std::max(first.maximum.y, second.maximum.y), std::max(first.maximum.z, second.maximum.z)}
    };
}
