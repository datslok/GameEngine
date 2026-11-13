#include "scene/mesh.h"

#include <cassert>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }

    Vec3 toVec3(const Vec4& value) {
        return Vec3{value.x, value.y, value.z};
    }

    bool refused(const std::vector<Vec2>& profile, int segments) {
        try {
            Mesh::lathe(profile, segments);
        } catch (const std::invalid_argument&) {
            return true;
        }

        return false;
    }

    // A bottle-like profile: a flat bottom, a straight body, a shoulder narrowing to a neck, a flat top.
    const std::vector<Vec2> bottleProfile{{0.03f, 0.0f}, {0.03f, 0.12f}, {0.02f, 0.16f}, {0.01f, 0.18f}, {0.01f, 0.23f}};
}

/*
* A surface of revolution: a profile of (radius, height) points from bottom to top spun round the y axis, closed by flat
* caps where its ends are off the axis. Every vertex lies on its profile point's circle; every face is wound
* counterclockwise seen from outside (its normal away from the axis on the sides, down at the bottom, up at the top);
* the smooth normals are unit length and point outwards; and the surface is closed, every edge shared by two triangles,
* so a light sees exactly one face turned towards it from each side.
*/
void testLatheMesh() {
    const int segments = 24;
    const Mesh bottle = Mesh::lathe(bottleProfile, segments);
    const std::size_t points = bottleProfile.size();

    // The rings, then a centre for each cap.
    assert(bottle.vertices.size() == points * segments + 2);
    // Two triangles a segment between neighbouring profile points, and one a segment in each cap.
    assert(bottle.triangles.size() == static_cast<std::size_t>(segments) * (2 * (points - 1) + 2));

    for (std::size_t ring = 0; ring < points; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            const Vec4& vertex = bottle.vertices[ring * segments + static_cast<std::size_t>(segment)];
            assert(nearlyEqual(std::sqrt(vertex.x * vertex.x + vertex.z * vertex.z), bottleProfile[ring].x));
            assert(nearlyEqual(vertex.y, bottleProfile[ring].y) && vertex.w == 1.0f);
        }
    }

    std::map<std::pair<std::size_t, std::size_t>, int> edges;

    for (const Triangle& triangle : bottle.triangles) {
        const Vec4& a = bottle.vertices[triangle.first];
        const Vec4& b = bottle.vertices[triangle.second];
        const Vec4& c = bottle.vertices[triangle.third];
        const Vec3 face = faceNormal(a, b, c);
        const Vec3 centre = (toVec3(a) + toVec3(b) + toVec3(c)) / 3.0f;
        assert(face.lengthSquared() > 0.0f);

        if (nearlyEqual(face.y, -1.0f)) {
            assert(nearlyEqual(centre.y, bottleProfile.front().y));
        } else if (nearlyEqual(face.y, 1.0f)) {
            assert(nearlyEqual(centre.y, bottleProfile.back().y));
        } else {
            assert(face.dot(Vec3{centre.x, 0.0f, centre.z}) > 0.0f);
        }

        const std::size_t corners[3] = {triangle.first, triangle.second, triangle.third};

        for (int corner = 0; corner < 3; ++corner) {
            assert(triangle.normals[corner].has_value() && triangle.uvs[corner].has_value());
            const Vec3& normal = *triangle.normals[corner];
            assert(nearlyEqual(normal.length(), 1.0f));
            assert(normal.dot(face) > 0.0f);

            const std::size_t from = corners[corner];
            const std::size_t to = corners[(corner + 1) % 3];
            ++edges[{std::min(from, to), std::max(from, to)}];
        }
    }

    for (const auto& [edge, count] : edges) {
        assert(count == 2);
    }

    // A straight side's normals are level (pointing straight out); the caps' point down and up.
    const Triangle& side = bottle.triangles[static_cast<std::size_t>(segments)];
    assert(nearlyEqual(side.normals[0]->y, 0.0f));

    // A profile ending on the axis needs no cap there, and its last band is one triangle a segment: a cone's tip.
    const Mesh cone = Mesh::lathe({{0.5f, 0.0f}, {0.0f, 1.0f}}, 8);
    assert(cone.triangles.size() == 8u * 2);

    for (const Triangle& triangle : cone.triangles) {
        assert(faceNormal(cone.vertices[triangle.first], cone.vertices[triangle.second], cone.vertices[triangle.third]).lengthSquared() > 0.0f);
    }

    // Too few points or segments, or a negative radius, are refused.
    assert(refused({{0.1f, 0.0f}}, 8));
    assert(refused(bottleProfile, 2));
    assert(refused({{0.1f, 0.0f}, {-0.1f, 1.0f}}, 8));
}
