#include "math/frustum.h"

#include <cmath>

namespace {
    struct Row {
        float x, y, z, w;
    };

    Row row(const Mat4& matrix, int index) {
        return Row{matrix.values[index][0], matrix.values[index][1], matrix.values[index][2], matrix.values[index][3]};
    }

    Row add(const Row& a, const Row& b, float sign) {
        return Row{a.x + sign * b.x, a.y + sign * b.y, a.z + sign * b.z, a.w + sign * b.w};
    }

    // Scale so the normal is unit length, which makes the plane's value a true distance (needed for the sphere test).
    Plane toPlane(const Row& r) {
        const float length = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
        const float scale = length > 0.0f ? 1.0f / length : 0.0f;
        return Plane{Vec3{r.x * scale, r.y * scale, r.z * scale}, r.w * scale};
    }
}

float Plane::signedDistance(const Vec3& point) const {
    return normal.dot(point) + distance;
}

/*
* A point p is on screen when its clip coordinates (x, y, z, w) = M p satisfy -w <= x <= w, -w <= y <= w, and
* -w <= z <= w (or 0 <= z <= w). Each inequality is one plane: for example x <= w means (row 3 - row 0) . p >= 0,
* so that row combination is the right side's plane, its normal pointing inwards. This works for any view-projection,
* perspective or orthographic, which is why shadow views can use it too.
*/
Frustum Frustum::fromClipMatrix(const Mat4& viewProjection, ClipDepth depth) {
    const Row x = row(viewProjection, 0);
    const Row y = row(viewProjection, 1);
    const Row z = row(viewProjection, 2);
    const Row w = row(viewProjection, 3);

    Frustum frustum;
    frustum.planes[0] = toPlane(add(w, x, 1.0f));  // left: -w <= x
    frustum.planes[1] = toPlane(add(w, x, -1.0f)); // right: x <= w
    frustum.planes[2] = toPlane(add(w, y, 1.0f));  // bottom
    frustum.planes[3] = toPlane(add(w, y, -1.0f)); // top
    frustum.planes[4] = depth == ClipDepth::ZeroToOne ? toPlane(z) : toPlane(add(w, z, 1.0f)); // near
    frustum.planes[5] = toPlane(add(w, z, -1.0f)); // far: z <= w

    return frustum;
}

/*
* A sphere is outside when it lies wholly beyond any one plane: its centre more than a radius outside. Six dot products.
*/
bool Frustum::intersectsSphere(const Vec3& centre, float radius) const {
    for (const Plane& plane : planes) {
        if (plane.signedDistance(centre) < -radius) {
            return false;
        }
    }

    return true;
}