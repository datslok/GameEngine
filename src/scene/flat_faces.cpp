#include "scene/flat_faces.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {
    // A mesh with more planes than this is curved or detailed, not a mirror; stopping early also keeps loading a big mesh
    // from comparing every triangle with every plane.
    constexpr std::size_t mostPlanes = 32;

    // The share of the mesh's area a plane must hold to count as a face: a box's sixth easily, a bevel's strip not.
    constexpr float smallestShare = 0.1f;

    // Two triangles share a plane when their normals are within about half a degree and their offsets agree to this
    // share of the mesh's size.
    constexpr float sameNormalCosine = 0.99995f;
    constexpr float sameOffsetShare = 1e-4f;

    struct Plane {
        Vec3 normal;
        float offset = 0.0f;
        float area = 0.0f;
        Aabb bounds;
    };

    Vec3 pointOf(const Vec4& vertex) {
        return Vec3{vertex.x, vertex.y, vertex.z};
    }

    void include(Aabb& bounds, const Vec3& point) {
        bounds.minimum = Vec3{std::min(bounds.minimum.x, point.x), std::min(bounds.minimum.y, point.y), std::min(bounds.minimum.z, point.z)};
        bounds.maximum = Vec3{std::max(bounds.maximum.x, point.x), std::max(bounds.maximum.y, point.y), std::max(bounds.maximum.z, point.z)};
    }
}

std::vector<FlatFace> findFlatFaces(const Mesh& mesh) {
    if (mesh.vertices.empty()) {
        return {};
    }

    Aabb whole{pointOf(mesh.vertices[0]), pointOf(mesh.vertices[0])};

    for (const Vec4& vertex : mesh.vertices) {
        include(whole, pointOf(vertex));
    }

    const float sameOffset = sameOffsetShare * whole.size().length();
    std::vector<Plane> planes;
    float totalArea = 0.0f;

    for (const Triangle& triangle : mesh.triangles) {
        const Vec3 a = pointOf(mesh.vertices[triangle.first]);
        const Vec3 b = pointOf(mesh.vertices[triangle.second]);
        const Vec3 c = pointOf(mesh.vertices[triangle.third]);
        const Vec3 areaNormal = (b - a).cross(c - a);
        const float doubleArea = areaNormal.length();

        if (!(doubleArea > 0.0f)) {
            continue;
        }

        const Vec3 normal = areaNormal * (1.0f / doubleArea);
        const float offset = normal.dot(a);
        const float area = 0.5f * doubleArea;
        totalArea += area;
        const auto same = std::find_if(planes.begin(), planes.end(), [&](const Plane& plane) {
            return plane.normal.dot(normal) > sameNormalCosine && std::abs(plane.offset - offset) <= sameOffset;
        });

        // A new plane moves the list, so the place to add to is worked out after it.
        std::size_t index = static_cast<std::size_t>(same - planes.begin());

        if (index == planes.size()) {
            if (planes.size() == mostPlanes) {
                return {};
            }

            planes.push_back(Plane{normal, offset, 0.0f, Aabb{a, a}});
        }

        Plane& plane = planes[index];
        plane.area += area;
        include(plane.bounds, a);
        include(plane.bounds, b);
        include(plane.bounds, c);
    }

    std::vector<FlatFace> faces;

    for (const Plane& plane : planes) {
        if (plane.area >= smallestShare * totalArea) {
            faces.push_back(FlatFace{plane.normal, plane.bounds});
        }
    }

    return faces;
}
