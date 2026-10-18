#pragma once

#include "math/aabb.h"
#include "math/mat4.h"
#include "physics/shapes.h"
#include "physics/triangle_mesh.h"

#include <memory>

/*
* Component: the shape an entity occupies for collision queries (picking, and later physics), in the entity's own
* coordinates, so it moves, turns and scales with the entity's Transform.
*
* `shape` is the coarse, cheap shape every query tests. `detail` optionally holds the exact triangles (in the same
* coordinates), for queries that ask for them: a hit on the shape is then confirmed against the triangles. The shape must
* enclose the triangles, because the shape's hit distance is used as a lower bound for theirs. The mesh is shared, not
* copied, so many entities with the same model (the demo's pillars) use one copy, and const, so no entity can change it
* under the others.
*/
struct Collider {
    Shape shape;
    std::shared_ptr<const TriangleMesh> detail;
};

/*
* The shape moved into the world by an entity's model matrix. Points (centres, capsule ends) move exactly. Radii grow by
* the largest scale along any axis, so an unevenly scaled sphere becomes the smallest sphere around its ellipsoid. A box
* becomes the axis-aligned box around the turned box, which is larger than it while turned.
*/
Shape worldShape(const Shape& local, const Mat4& model);

// The smallest box around a mesh's corners. An empty mesh gives a zero box at the origin.
Aabb triangleMeshBounds(const TriangleMesh& mesh);

// A collider whose shape is the box around the mesh and whose detail is the mesh itself.
Collider meshCollider(std::shared_ptr<const TriangleMesh> mesh);
