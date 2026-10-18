#pragma once

#include "math/mat4.h"
#include "math/ray.h"
#include "physics/shapes.h"
#include "physics/triangle_mesh.h"

#include <limits>
#include <optional>

// Where a ray first meets something.
struct RayHit {
    float distance = 0.0f; // from the ray's origin, in world units even when the direction is not unit length
    Vec3 point;
    Vec3 normal;           // unit length, always pointing back against the ray (out of the surface the ray came through)
};

constexpr float unlimitedRayDistance = std::numeric_limits<float>::infinity();

/*
* The first place a ray meets a shape or a triangle mesh within maxDistance, or nothing. A ray that starts inside a shape
* hits at once (distance 0, normal opposite the ray), so a query never looks straight through what it starts in. Triangles
* are hit from either side. A ray with a zero direction hits nothing.
*/

std::optional<RayHit> raycast(const Ray& ray, const Sphere& sphere, float maxDistance = unlimitedRayDistance);
std::optional<RayHit> raycast(const Ray& ray, const Capsule& capsule, float maxDistance = unlimitedRayDistance);
std::optional<RayHit> raycast(const Ray& ray, const Aabb& box, float maxDistance = unlimitedRayDistance);
std::optional<RayHit> raycast(const Ray& ray, const Shape& shape, float maxDistance = unlimitedRayDistance);

// Tests every triangle (a broad phase will cut that down later) and keeps the nearest hit.
std::optional<RayHit> raycast(const Ray& ray, const TriangleMesh& mesh, float maxDistance = unlimitedRayDistance);

// The same, for a mesh stored in an object's own coordinates and placed by its model matrix. The ray is carried into the
// mesh's coordinates instead of the mesh into the world; the hit comes back in world units and directions. A matrix that
// squashes space flat hits nothing.
std::optional<RayHit> raycast(const Ray& ray, const TriangleMesh& mesh, const Mat4& model, float maxDistance = unlimitedRayDistance);
