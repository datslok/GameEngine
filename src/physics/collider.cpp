#include "physics/collider.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <variant>

namespace {
    /*
    * How much the matrix stretches its most stretched axis: the longest of the three columns of its 3x3 part (column j is
    * where the unit vector along axis j ends up). For a Transform's matrix (rotation times scale) that is exactly the largest
    * scale, so a sphere scaled by it is the smallest sphere around the stretched one.
    */
    float largestStretch(const Mat4& model) {
        float largest = 0.0f;
        for (int column = 0; column < 3; ++column) {
            const Vec3 axis{model.values[0][column], model.values[1][column], model.values[2][column]};
            largest = std::max(largest, axis.length());
        }
        return largest;
    }

    Shape moveShape(const Sphere& sphere, const Mat4& model) {
        return Sphere{transformPoint(model, sphere.centre), sphere.radius * largestStretch(model)};
    }

    Shape moveShape(const Capsule& capsule, const Mat4& model) {
        return Capsule{transformPoint(model, capsule.a), transformPoint(model, capsule.b), capsule.radius * largestStretch(model)};
    }

    /*
    * A corner is centre + (+-h_x, +-h_y, +-h_z) with each sign free. After the matrix, its offset along world axis i is
    * sum_j A_ij (+-h_j), largest when every sign matches A_ij's: sum_j |A_ij| h_j. So that is the half-size of the box around
    * the turned box, and its centre is simply the moved centre.
    */
    Shape moveShape(const Aabb& box, const Mat4& model) {
        const Vec3 centre = transformPoint(model, box.centre());
        const Vec3 half = box.size() * 0.5f;

        float extent[3];
        for (int row = 0; row < 3; ++row) {
            extent[row] = std::abs(model.values[row][0]) * half.x
                + std::abs(model.values[row][1]) * half.y
                + std::abs(model.values[row][2]) * half.z;
        }

        const Vec3 worldHalf{extent[0], extent[1], extent[2]};
        return Aabb{centre - worldHalf, centre + worldHalf};
    }
}

/*
* std::visit picks the moveShape above that matches what the shape holds.
*/
Shape worldShape(const Shape& local, const Mat4& model) {
    return std::visit([&model](const auto& shape) { return moveShape(shape, model); }, local);
}

Aabb triangleMeshBounds(const TriangleMesh& mesh) {
    if (mesh.positions.empty()) {
        return Aabb{};
    }

    Aabb bounds{mesh.positions.front(), mesh.positions.front()};
    for (const Vec3& position : mesh.positions) {
        bounds.minimum = Vec3{std::min(bounds.minimum.x, position.x), std::min(bounds.minimum.y, position.y), std::min(bounds.minimum.z, position.z)};
        bounds.maximum = Vec3{std::max(bounds.maximum.x, position.x), std::max(bounds.maximum.y, position.y), std::max(bounds.maximum.z, position.z)};
    }
    return bounds;
}

/*
* The box around the mesh always encloses it, which is what the two-stage query needs.
*/
Collider meshCollider(std::shared_ptr<const TriangleMesh> mesh) {
    const Aabb bounds = mesh ? triangleMeshBounds(*mesh) : Aabb{};
    return Collider{bounds, std::move(mesh)};
}
