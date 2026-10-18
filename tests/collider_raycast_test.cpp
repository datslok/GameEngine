#include "math/transform.h"
#include "physics/collider_raycast.h"

#include <cassert>
#include <cmath>
#include <memory>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }

    void assertVector(const Vec3& actual, const Vec3& expected) {
        assert(nearlyEqual(actual.x, expected.x));
        assert(nearlyEqual(actual.y, expected.y));
        assert(nearlyEqual(actual.z, expected.z));
    }

    Entity spawn(World& world, const Vec3& position, const Collider& collider, const Vec3& scale = Vec3{1.0f, 1.0f, 1.0f}) {
        const Entity entity = world.create();
        Transform transform;
        transform.position = position;
        transform.scale = scale;
        world.add(entity, transform);
        world.add(entity, collider);
        return entity;
    }

    // Looking down -z from z = 10 along the x axis line.
    const Ray forward{Vec3{0.0f, 0.0f, 10.0f}, Vec3{0.0f, 0.0f, -1.0f}};

    void testNearestShape() {
        World world;
        const Collider ball{Sphere{Vec3{0.0f, 0.0f, 0.0f}, 1.0f}, nullptr};

        // Created far one first, so storage order is not what decides.
        const Entity far = spawn(world, Vec3{0.0f, 0.0f, -5.0f}, ball);
        const Entity near = spawn(world, Vec3{0.0f, 0.0f, 0.0f}, ball);
        spawn(world, Vec3{5.0f, 0.0f, 0.0f}, ball); // off to the side, never in the way

        const auto hit = raycastColliders(world, forward, RaycastDetail::Shapes);
        assert(hit.has_value());
        assert(hit->entity == near);
        assert(nearlyEqual(hit->hit.distance, 9.0f));
        assertVector(hit->hit.normal, Vec3{0.0f, 0.0f, 1.0f});

        // Out of reach of the near one is out of reach of both.
        assert(!raycastColliders(world, forward, RaycastDetail::Shapes, 8.0f));

        // Without the near one, the far one is next.
        world.destroy(near);
        assert(raycastColliders(world, forward, RaycastDetail::Shapes)->entity == far);

        // Missing entirely.
        assert(!raycastColliders(world, Ray{Vec3{0.0f, 5.0f, 10.0f}, Vec3{0.0f, 0.0f, -1.0f}}, RaycastDetail::Shapes));

        // A collider without a Transform has no place in the world, so it is never hit.
        World unplaced;
        unplaced.add(unplaced.create(), ball);
        assert(!raycastColliders(unplaced, forward, RaycastDetail::Shapes));
    }

    // Two triangles in the z = 0 plane with a gap between them along x = 0, like the hole in a teapot's handle.
    std::shared_ptr<TriangleMesh> splitPanel() {
        auto mesh = std::make_shared<TriangleMesh>();
        mesh->positions = {
            Vec3{-2.0f, -1.0f, 0.0f}, Vec3{-0.5f, -1.0f, 0.0f}, Vec3{-0.5f, 1.0f, 0.0f},
            Vec3{0.5f, -1.0f, 0.0f}, Vec3{2.0f, -1.0f, 0.0f}, Vec3{0.5f, 1.0f, 0.0f}
        };
        mesh->indices = {0, 1, 2, 3, 4, 5};
        return mesh;
    }

    void testRefinement() {
        World world;

        // The panel's collider is a box around both triangles, gap included; behind it stands a plain ball.
        const Collider panel{Aabb{Vec3{-2.0f, -1.0f, -0.5f}, Vec3{2.0f, 1.0f, 0.5f}}, splitPanel()};
        const Entity front = spawn(world, Vec3{0.0f, 0.0f, 0.0f}, panel);
        const Entity behind = spawn(world, Vec3{0.0f, 0.0f, -5.0f}, Collider{Sphere{Vec3{0.0f, 0.0f, 0.0f}, 1.0f}, nullptr});

        // Through the gap: the box alone says the panel, the triangles say the ball behind it.
        assert(raycastColliders(world, forward, RaycastDetail::Shapes)->entity == front);
        const auto throughGap = raycastColliders(world, forward, RaycastDetail::Triangles);
        assert(throughGap.has_value());
        assert(throughGap->entity == behind);
        assert(nearlyEqual(throughGap->hit.distance, 14.0f));

        // On a triangle: the panel, at its surface (z = 0), not at the front of its box (z = 0.5).
        const Ray onPanel{Vec3{-1.0f, -0.5f, 10.0f}, Vec3{0.0f, 0.0f, -1.0f}};
        const auto panelHit = raycastColliders(world, onPanel, RaycastDetail::Triangles);
        assert(panelHit->entity == front);
        assert(nearlyEqual(panelHit->hit.distance, 10.0f));
        assertVector(panelHit->hit.normal, Vec3{0.0f, 0.0f, 1.0f});
    }

    void testScaledRefinement() {
        // The panel stretched 3 times in x and 2 times in z, then turned to face +x by a model matrix that is not just a
        // translation. Distances and normals must come back in world units and world directions.
        World world;
        const Collider panel{Aabb{Vec3{-2.0f, -1.0f, -0.5f}, Vec3{2.0f, 1.0f, 0.5f}}, splitPanel()};
        const Entity stretched = spawn(world, Vec3{0.0f, 0.0f, 0.0f}, panel, Vec3{3.0f, 1.0f, 2.0f});

        // The left triangle now spans x = -6..-1.5. A ray down -z at x = -3 hits it at z = 0, 10 away.
        const Ray onStretched{Vec3{-3.0f, -0.5f, 10.0f}, Vec3{0.0f, 0.0f, -1.0f}};
        const auto hit = raycastColliders(world, onStretched, RaycastDetail::Triangles);
        assert(hit.has_value());
        assert(hit->entity == stretched);
        assert(nearlyEqual(hit->hit.distance, 10.0f));
        assertVector(hit->hit.point, Vec3{-3.0f, -0.5f, 0.0f});
        assertVector(hit->hit.normal, Vec3{0.0f, 0.0f, 1.0f});

        // The gap is now x = -1.5..1.5: x = 1 passes through.
        assert(!raycastColliders(world, Ray{Vec3{1.0f, 0.0f, 10.0f}, Vec3{0.0f, 0.0f, -1.0f}}, RaycastDetail::Triangles));

        // A slanted triangle under an uneven scale, through the single-mesh overload: the normal must be the true world
        // surface normal, which is not the local normal scaled. Locally the triangle lies in the plane x + y = 0 (normal along
        // (1, 1, 0)). Halving heights maps a local point (x, y) to (x, y / 2), so world points satisfy x + 2y = 0, whose normal
        // is along (1, 2, 0). Scaling the local normal instead would give (1, 0.5, 0), tilted the wrong way.
        TriangleMesh slope;
        slope.positions = {Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{1.0f, -1.0f, 0.0f}};
        slope.indices = {0, 1, 2};
        const Mat4 halfHeight = Mat4::scaling(1.0f, 0.5f, 1.0f);
        const auto slopeHit = raycast(Ray{Vec3{0.2f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}}, slope, halfHeight);
        assert(slopeHit.has_value());
        assertVector(slopeHit->point, Vec3{0.2f, -0.1f, 0.0f});
        assert(nearlyEqual(slopeHit->distance, 5.1f));
        assertVector(slopeHit->normal, Vec3{1.0f, 2.0f, 0.0f}.normalized());

        // A model matrix with no inverse (squashed flat) is never hit, rather than dividing by zero.
        assert(!raycast(Ray{Vec3{0.2f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}}, slope, Mat4::scaling(1.0f, 0.0f, 1.0f)));
    }
}

void testColliderRaycast() {
    testNearestShape();
    testRefinement();
    testScaledRefinement();
}
