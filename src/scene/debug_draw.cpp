#include "scene/debug_draw.h"

#include <algorithm>
#include <cmath>
#include <numbers>

void DebugDraw::line(const Vec3& from, const Vec3& to, const Vec3& colour) {
    lines.push_back(DebugLine{from, to, colour});
}

/*
* Corner i takes the maximum on each axis whose bit is set (x: 1, y: 2, z: 4). Two corners share an edge when they differ
* in exactly one bit, so each corner connects to the corners with one more bit set: 12 edges, each drawn once.
*/
void DebugDraw::box(const ModelBounds& bounds, const Mat4& transform, const Vec3& colour) {
    Vec3 corners[8];

    for (int i = 0; i < 8; ++i) {
        const Vec4 local{
            (i & 1) ? bounds.maximum.x : bounds.minimum.x,
            (i & 2) ? bounds.maximum.y : bounds.minimum.y,
            (i & 4) ? bounds.maximum.z : bounds.minimum.z,
            1.0f
        };
        const Vec4 placed = transform * local;
        corners[i] = Vec3{placed.x, placed.y, placed.z};
    }

    for (int i = 0; i < 8; ++i) {
        for (int bit : {1, 2, 4}) {
            if ((i & bit) == 0) {
                line(corners[i], corners[i | bit], colour);
            }
        }
    }
}

void DebugDraw::arc(const Vec3& centre, const Vec3& axisA, const Vec3& axisB, float radius, float startAngle, float sweepAngle, const Vec3& colour, int segments) {
    const int count = std::max(segments, 1);

    const auto pointAt = [&](int step) {
        const float angle = startAngle + sweepAngle * static_cast<float>(step) / static_cast<float>(count);
        return centre + axisA * (radius * std::cos(angle)) + axisB * (radius * std::sin(angle));
    };

    for (int step = 0; step < count; ++step) {
        line(pointAt(step), pointAt(step + 1), colour);
    }
}

void DebugDraw::sphere(const Vec3& centre, float radius, const Vec3& colour, int segments) {
    const float fullTurn = 2.0f * std::numbers::pi_v<float>;
    const Vec3 x{1.0f, 0.0f, 0.0f};
    const Vec3 y{0.0f, 1.0f, 0.0f};
    const Vec3 z{0.0f, 0.0f, 1.0f};

    arc(centre, x, y, radius, 0.0f, fullTurn, colour, segments);
    arc(centre, y, z, radius, 0.0f, fullTurn, colour, segments);
    arc(centre, z, x, radius, 0.0f, fullTurn, colour, segments);
}

/*
* Two unit vectors perpendicular to the axis (side and front) span the end circles. The half circles over each end
* sweep from one side, over the axis direction, to the other, so together the outline shows the rounded ends.
*/
void DebugDraw::capsule(const Vec3& a, const Vec3& b, float radius, const Vec3& colour, int segments) {
    const Vec3 axis = b - a;

    if (axis.lengthSquared() < 1e-12f) {
        sphere(a, radius, colour, segments);
        return;
    }

    const float pi = std::numbers::pi_v<float>;
    const Vec3 up = axis.normalized();
    const Vec3 reference = std::abs(up.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 side = up.cross(reference).normalized();
    const Vec3 front = side.cross(up);
    const int halfSegments = std::max(segments / 2, 1);

    arc(a, side, front, radius, 0.0f, 2.0f * pi, colour, segments);
    arc(b, side, front, radius, 0.0f, 2.0f * pi, colour, segments);

    for (const Vec3& offset : {side, side * -1.0f, front, front * -1.0f}) {
        line(a + offset * radius, b + offset * radius, colour);
    }

    arc(b, side, up, radius, 0.0f, pi, colour, halfSegments);
    arc(b, front, up, radius, 0.0f, pi, colour, halfSegments);
    arc(a, side, up, radius, pi, pi, colour, halfSegments);
    arc(a, front, up, radius, pi, pi, colour, halfSegments);
}

void DebugDraw::clear() {
    lines.clear();
}

const std::vector<DebugLine>& DebugDraw::getLines() const {
    return lines;
}