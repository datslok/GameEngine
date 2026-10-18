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

// One triangle, hit from either side; the normal faces the ray.
std::optional<RayHit> raycastTriangle(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c,
                                      float maxDistance = unlimitedRayDistance);

/*
* The first place a moving sphere touches one triangle (from either side) as its centre moves by motion, or nothing.
* The centre touches when it reaches the triangle grown by the radius: the triangle pushed out both ways along its normal,
* plus a capsule of that radius round each edge (whose round ends cover the corners). So the sweep is a raycast of the
* centre against those two triangles and three capsules. The hit's distance is how far the centre moved, its point is the
* centre at that moment, and its normal points from the touched point on the triangle towards the centre.
*
* A sphere already touching or overlapping the triangle hits at distance 0 (with the contact's normal) if it is moving
* into it, and ignores it if moving along or away from it, so a sphere resting on a floor can still roll off it.
*/
std::optional<RayHit> sweepSphere(const Sphere& sphere, const Vec3& motion, const Vec3& a, const Vec3& b, const Vec3& c);

// Tests every triangle (a broad phase will cut that down later) and keeps the nearest hit.
std::optional<RayHit> raycast(const Ray& ray, const TriangleMesh& mesh, float maxDistance = unlimitedRayDistance);

// The same, for a mesh stored in an object's own coordinates and placed by its model matrix. The ray is carried into the
// mesh's coordinates instead of the mesh into the world; the hit comes back in world units and directions. A matrix that
// squashes space flat hits nothing.
std::optional<RayHit> raycast(const Ray& ray, const TriangleMesh& mesh, const Mat4& model, float maxDistance = unlimitedRayDistance);
