#pragma once

#include "math/mat4.h"
#include "math/vec3.h"

#include <array>

// How a projection maps depth: OpenGL style (-w..w, what Mat4::perspective gives) or 0..w (what GPUs use).
enum class ClipDepth {
    NegativeOneToOne,
    ZeroToOne
};

// A plane as the points p with normal.dot(p) + distance = 0. The normal is unit length and points to the inside.
struct Plane {
    Vec3 normal;
    float distance = 0.0f;

    // How far a point is on the inside (positive) or outside (negative).
    float signedDistance(const Vec3& point) const;
};

/*
* The space a camera sees: a truncated pyramid for perspective, a box for orthographic, bounded by six planes
* (left, right, bottom, top, near, far). Used to skip drawing things that cannot be seen.
*/
struct Frustum {
    Plane planes[6];

    // The six sides of any view-projection matrix: the camera's, or a shadow view's.
    static Frustum fromClipMatrix(const Mat4& viewProjection, ClipDepth depth);

    // False only when the sphere is entirely outside one of the planes. Near a corner it can say true for a sphere that is
    // just outside, which only means a little wasted drawing, never something missing.
    bool intersectsSphere(const Vec3& centre, float radius) const;

    // The eight corners, where three sides meet (one of left/right, bottom/top and near/far each).
    std::array<Vec3, 8> corners() const;

    // False only when the two views cannot share any point: one lies wholly outside a side of the other. Like the sphere
    // test it can say true for two views that just miss each other past a corner, never false for two that overlap.
    bool mayIntersect(const Frustum& other) const;
};