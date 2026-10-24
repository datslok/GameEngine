#pragma once

#include "math/vec3.h"

/*
* An axis-aligned box: every point whose coordinates all lie between minimum and maximum. Used for mesh bounds and as a
* collision shape. Its faces never rotate with anything, which is what makes tests against it cheap (each axis separately).
*/
struct Aabb {
    Vec3 minimum;
    Vec3 maximum;

    Vec3 centre() const;
    Vec3 size() const;

    // Whether the point lies inside or on the box's faces.
    bool contains(const Vec3& point) const;
};

// The smallest box holding both.
Aabb enclosing(const Aabb& first, const Aabb& second);
