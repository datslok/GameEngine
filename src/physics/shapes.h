#pragma once

#include "math/aabb.h"
#include "math/vec3.h"

#include <variant>

/*
* Collision shapes, all in one shared (usually world) space. They are deliberately simple: a sphere and a capsule are a
* point and a segment grown by a radius, so tests between them reduce to closest points (math/closest_points), and the
* box never rotates. Together they cover most game colliders: projectiles (sphere), characters (capsule), crates and
* level blocks (box). Rotated boxes and triangle meshes come later.
*/

struct Sphere {
    Vec3 centre;
    float radius = 0.0f;
};

// Every point within radius of the segment from a to b. Upright characters use a vertical one; a == b makes it a sphere.
struct Capsule {
    Vec3 a;
    Vec3 b;
    float radius = 0.0f;
};

// Any one of the shapes, for code that handles colliders without knowing their kind (the broad phase, picking).
using Shape = std::variant<Sphere, Capsule, Aabb>;
