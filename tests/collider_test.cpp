#include "physics/collider.h"

#include <cassert>
#include <cmath>
#include <memory>
#include <numbers>
#include <variant>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }

    void assertVector(const Vec3& actual, const Vec3& expected) {
        assert(nearlyEqual(actual.x, expected.x));
        assert(nearlyEqual(actual.y, expected.y));
        assert(nearlyEqual(actual.z, expected.z));
    }

    void testWorldShapes() {
        const Mat4 moved = Mat4::translation(10.0f, 0.0f, 0.0f);
        const Mat4 stretched = Mat4::scaling(1.0f, 3.0f, 2.0f);

        // A sphere's centre moves with the entity; its radius grows by the largest stretch, so it still encloses the ellipsoid.
        const Shape sphere = worldShape(Sphere{Vec3{0.0f, 1.0f, 0.0f}, 0.5f}, moved * stretched);
        assert(std::holds_alternative<Sphere>(sphere));
        assertVector(std::get<Sphere>(sphere).centre, Vec3{10.0f, 3.0f, 0.0f});
        assert(nearlyEqual(std::get<Sphere>(sphere).radius, 1.5f));

        // A capsule's two ends move like points, and its radius like the sphere's.
        const Shape capsule = worldShape(Capsule{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 0.5f}, moved * stretched);
        assert(std::holds_alternative<Capsule>(capsule));
        assertVector(std::get<Capsule>(capsule).a, Vec3{10.0f, 0.0f, 0.0f});
        assertVector(std::get<Capsule>(capsule).b, Vec3{10.0f, 3.0f, 0.0f});
        assert(nearlyEqual(std::get<Capsule>(capsule).radius, 1.5f));

        // A box that is only moved and stretched stays exact.
        const Aabb unit{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}};
        const Shape box = worldShape(unit, moved * stretched);
        assert(std::holds_alternative<Aabb>(box));
        assertVector(std::get<Aabb>(box).minimum, Vec3{9.0f, -3.0f, -2.0f});
        assertVector(std::get<Aabb>(box).maximum, Vec3{11.0f, 3.0f, 2.0f});

        // Turned 45 degrees about y, the cube's enclosing box is sqrt(2) wide each way in x and z, and unchanged in y.
        const float turned = std::sqrt(2.0f);
        const Shape spun = worldShape(unit, moved * Mat4::rotationY(std::numbers::pi_v<float> / 4.0f));
        assertVector(std::get<Aabb>(spun).minimum, Vec3{10.0f - turned, -1.0f, -turned});
        assertVector(std::get<Aabb>(spun).maximum, Vec3{10.0f + turned, 1.0f, turned});

        // An off-centre box keeps its offset from the entity, turned with it: its centre at local (2, 0, 0) ends up at (0, 0, -2) after
        // a quarter turn about y (which takes +x to -z).
        const Aabb offset{Vec3{1.0f, -1.0f, -1.0f}, Vec3{3.0f, 1.0f, 1.0f}};
        const Shape quarter = worldShape(offset, Mat4::rotationY(std::numbers::pi_v<float> / 2.0f));
        assertVector(std::get<Aabb>(quarter).centre(), Vec3{0.0f, 0.0f, -2.0f});
        assertVector(std::get<Aabb>(quarter).size(), Vec3{2.0f, 2.0f, 2.0f});
    }

    void testMeshCollider() {
        auto mesh = std::make_shared<TriangleMesh>();
        mesh->positions = {Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 1.0f, 0.0f}, Vec3{-1.0f, 3.0f, 4.0f}};
        mesh->indices = {0, 1, 2};

        const Aabb bounds = triangleMeshBounds(*mesh);
        assertVector(bounds.minimum, Vec3{-1.0f, 0.0f, 0.0f});
        assertVector(bounds.maximum, Vec3{2.0f, 3.0f, 4.0f});

        // A collider made from a mesh: its box encloses the mesh, and the mesh is kept (shared, not copied) for exact queries.
        const Collider collider = meshCollider(mesh);
        assert(std::holds_alternative<Aabb>(collider.shape));
        assertVector(std::get<Aabb>(collider.shape).minimum, bounds.minimum);
        assertVector(std::get<Aabb>(collider.shape).maximum, bounds.maximum);
        assert(collider.detail.get() == mesh.get());
    }
}

void testCollider() {
    testWorldShapes();
    testMeshCollider();
}
