#pragma once

#include "physics/shapes.h"

/*
* Whether two shapes share any point. Shapes that only touch count as overlapping, so a character standing exactly on a
* floor is in contact with it. Every pair works in either order.
*/

bool overlaps(const Sphere& first, const Sphere& second);
bool overlaps(const Sphere& sphere, const Capsule& capsule);
bool overlaps(const Sphere& sphere, const Aabb& box);
bool overlaps(const Capsule& first, const Capsule& second);
bool overlaps(const Capsule& capsule, const Aabb& box);
bool overlaps(const Aabb& first, const Aabb& second);

bool overlaps(const Capsule& capsule, const Sphere& sphere);
bool overlaps(const Aabb& box, const Sphere& sphere);
bool overlaps(const Aabb& box, const Capsule& capsule);

// Picks the right test for whatever kinds the two shapes hold.
bool overlaps(const Shape& first, const Shape& second);
