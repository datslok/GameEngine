#include "scene/smooth_normals.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
    // A position's exact bits, so corners only share a normal when they are at exactly the same place.
    using PositionKey = std::array<std::uint32_t, 3>;

    // Adding +0.0 turns -0.0 into +0.0, so the two zeros give the same key.
    PositionKey keyOf(const Vec4& position) {
        return PositionKey{
            std::bit_cast<std::uint32_t>(position.x + 0.0f),
            std::bit_cast<std::uint32_t>(position.y + 0.0f),
            std::bit_cast<std::uint32_t>(position.z + 0.0f)
        };
    }

    // One face's share of the normal at one of its corners.
    struct Contribution {
        Vec3 faceNormal;
        float weight;
    };

    Vec3 toVec3(const Vec4& value) {
        return Vec3{value.x, value.y, value.z};
    }

    /*
    * The angle of the triangle at a corner. Weighting by it makes the result depend on the shape of the surface,
    * not on how it was cut into triangles: a fan of thin slivers counts as much as one wide triangle covering the same wedge.
    */
    float cornerAngle(const Vec4& corner, const Vec4& next, const Vec4& previous) {
        const Vec3 toNext = (toVec3(next) - toVec3(corner)).normalized();
        const Vec3 toPrevious = (toVec3(previous) - toVec3(corner)).normalized();
        return std::acos(std::clamp(toNext.dot(toPrevious), -1.0f, 1.0f));
    }
}

/*
* Two passes: first collect each face's normal and weight at every position it touches, then give each corner the
* weighted average of the faces at its position that are within the crease angle of its own face. A face always passes
* its own test, so the average is never empty.
*/
void generateSmoothNormals(Mesh& mesh, float creaseAngle) {
    const float minimumAlignment = std::cos(creaseAngle);

    std::vector<Vec3> faceNormals;
    faceNormals.reserve(mesh.triangles.size());

    std::map<PositionKey, std::vector<Contribution>> contributions;

    for (const Triangle& triangle : mesh.triangles) {
        const std::array<std::size_t, 3> corners{triangle.first, triangle.second, triangle.third};

        for (const std::size_t corner : corners) {
            if (corner >= mesh.vertices.size()) {
                throw std::out_of_range("Mesh triangle references a missing vertex");
            }
        }

        const Vec4& a = mesh.vertices[corners[0]];
        const Vec4& b = mesh.vertices[corners[1]];
        const Vec4& c = mesh.vertices[corners[2]];
        const Vec3 normal = faceNormal(a, b, c);
        faceNormals.push_back(normal);

        // A triangle with no area has no direction to contribute.
        if (normal.lengthSquared() == 0.0f) {
            continue;
        }

        contributions[keyOf(a)].push_back(Contribution{normal, cornerAngle(a, b, c)});
        contributions[keyOf(b)].push_back(Contribution{normal, cornerAngle(b, c, a)});
        contributions[keyOf(c)].push_back(Contribution{normal, cornerAngle(c, a, b)});
    }

    for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
        Triangle& triangle = mesh.triangles[index];
        const Vec3& ownNormal = faceNormals[index];

        if (ownNormal.lengthSquared() == 0.0f) {
            continue;
        }

        const std::array<std::size_t, 3> corners{triangle.first, triangle.second, triangle.third};

        for (std::size_t corner = 0; corner < 3; ++corner) {
            if (triangle.normals[corner].has_value()) {
                continue;
            }

            Vec3 sum{0.0f, 0.0f, 0.0f};

            for (const Contribution& contribution : contributions[keyOf(mesh.vertices[corners[corner]])]) {
                if (ownNormal.dot(contribution.faceNormal) >= minimumAlignment) {
                    sum = sum + contribution.faceNormal * contribution.weight;
                }
            }

            triangle.normals[corner] = sum.lengthSquared() > 0.0f ? sum.normalized() : ownNormal;
        }
    }
}
