#pragma once

#include "cloth/cloth_particles.h"
#include "ecs/world.h"
#include "physics/shapes.h"

#include <vector>

struct ClothSettings;

// How far a point is from a shape's surface (negative inside) and the way out: the outward normal at the closest point.
struct SurfacePoint {
    float distance = 0.0f;
    Vec3 normal{0.0f, 1.0f, 0.0f};
};

/*
* Exact for every phase 5 shape. Spheres and capsules: from the centre (or the closest point of the capsule's segment)
* out through the point. Boxes: outside, from the closest point of the box; inside, out through the nearest face. A point
* exactly at a sphere's centre or on a capsule's segment has no way out of its own, so it gets straight up.
*/
SurfacePoint surfacePoint(const Shape& shape, const Vec3& point);

/*
* The cloth against still bodies, one-sided constraints with no give, solved after the cloth's own: each free point
* inside a shape, or closer than settings.thickness, is pushed out along the normal to that distance. Then friction on
* the positions (Macklin et al. 2014), Coulomb's law: the slide along the surface during this substep (from where the
* point began it) is undone when shorter than staticFriction times the push, and otherwise shortened by kineticFriction
* times the push. Pinned points are never moved. On the caller: shared among threads it was measured slower (each
* point's test is a few nanoseconds, less than handing it out costs).
*/
void solveContacts(ClothParticles& particles, const std::vector<Shape>& shapes, const ClothSettings& settings);

// Every Collider's shape placed in the world by its entity's Transform: what cloth collides with.
std::vector<Shape> worldColliderShapes(const World& world);
