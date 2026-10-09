#include "scene/mesh.h"

#include <cassert>
#include <cmath>
#include <stdexcept>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }

    Vec3 toVec3(const Vec4& value) {
        return Vec3{value.x, value.y, value.z};
    }

    bool throwsInvalidArgument(int segments, int rings) {
        try {
            Mesh::sphere(segments, rings);
        }
        catch (const std::invalid_argument&) {
            return true;
        }

        return false;
    }
}

/*
* A UV sphere: a grid of longitude segments and latitude rings, with single triangles around each pole.
*/
void testSphereMesh() {
    const int segments = 16;
    const int rings = 8;
    const Mesh sphere = Mesh::sphere(segments, rings);

    // Two triangles per grid cell, but only one per segment in the ring touching each pole.
    assert(sphere.triangles.size() == static_cast<std::size_t>(segments * (2 * rings - 2)));

    // A unit sphere: every vertex is one unit from the centre.
    for (const Vec4& vertex : sphere.vertices) {
        assert(nearlyEqual(toVec3(vertex).length(), 1.0f));
        assert(nearlyEqual(vertex.w, 1.0f));
    }

    for (const Triangle& triangle : sphere.triangles) {
        const Vec4& a = sphere.vertices[triangle.first];
        const Vec4& b = sphere.vertices[triangle.second];
        const Vec4& c = sphere.vertices[triangle.third];

        // Wound counterclockwise seen from outside, so the face normal points away from the centre.
        const Vec3 centre = (toVec3(a) + toVec3(b) + toVec3(c)) / 3.0f;
        assert(faceNormal(a, b, c).dot(centre) > 0.0f);

        // Smooth: each corner's normal is the outward direction at that point, so no smoothing pass is needed.
        const Vec4* corners[3] = {&a, &b, &c};

        for (int corner = 0; corner < 3; ++corner) {
            assert(triangle.normals[corner].has_value());
            const Vec3 expected = toVec3(*corners[corner]);
            const Vec3& normal = *triangle.normals[corner];
            assert(nearlyEqual(normal.x, expected.x) && nearlyEqual(normal.y, expected.y) && nearlyEqual(normal.z, expected.z));
        }
    }

    // Fewer than three segments or two rings cannot enclose any space.
    assert(throwsInvalidArgument(2, 8));
    assert(throwsInvalidArgument(16, 1));
}
