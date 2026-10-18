#pragma once

#include "physics/shapes.h"

#include <cstdint>
#include <optional>

/*
* Where a shape presses into a surface, and how to get it out: move it `depth` along `normal` (a unit vector pointing
* from the surface towards the shape, like a normal force) and it no longer overlaps. `point` is on the surface.
* `triangle` says which triangle of a level, when the contact came from one.
*/
struct Contact {
    Vec3 point;
    Vec3 normal;
    float depth = 0.0f;
    std::uint32_t triangle = 0;

    // The surface's own unit normal, turned to the shape's side. Equal to normal when the shape presses on the face; different
    // when it presses on an edge or a corner, which tells a ledge's edge from a slope.
    Vec3 faceNormal;
};

/*
* The contact between a capsule (or sphere) and one triangle, or nothing if they do not touch. Touching counts, with
* depth 0. Triangles act from both sides: the normal points from the triangle's nearest point towards the capsule's core.
* When the core passes right through the triangle, there is no such direction, so the triangle's face normal is used,
* on the side where the core's middle is, and the depth is enough to bring the core's far end back out plus the radius.
*/
std::optional<Contact> contact(const Capsule& capsule, const Vec3& a, const Vec3& b, const Vec3& c);
std::optional<Contact> contact(const Sphere& sphere, const Vec3& a, const Vec3& b, const Vec3& c);
