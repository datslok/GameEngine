#include "physics/raycast.h"

#include <cassert>
#include <cmath>
#include <optional>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }

    void assertVector(const Vec3& actual, const Vec3& expected) {
        assert(nearlyEqual(actual.x, expected.x));
        assert(nearlyEqual(actual.y, expected.y));
        assert(nearlyEqual(actual.z, expected.z));
    }

    void assertHit(const std::optional<RayHit>& hit, float distance, const Vec3& point, const Vec3& normal) {
        assert(hit.has_value());
        assert(nearlyEqual(hit->distance, distance));
        assertVector(hit->point, point);
        assertVector(hit->normal, normal);
    }

    void testSphere() {
        const Sphere sphere{Vec3{0.0f, 0.0f, 0.0f}, 1.0f};

        // Straight at it: the near side, facing back at the ray.
        assertHit(raycast(Ray{Vec3{0.0f, 0.0f, -5.0f}, Vec3{0.0f, 0.0f, 1.0f}}, sphere),
            4.0f, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 0.0f, -1.0f});

        // A direction that is not unit length still reports distances in world units.
        assertHit(raycast(Ray{Vec3{0.0f, 0.0f, -5.0f}, Vec3{0.0f, 0.0f, 3.0f}}, sphere),
            4.0f, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 0.0f, -1.0f});

        // Passing beside it, pointing away from it, and a zero direction all miss.
        assert(!raycast(Ray{Vec3{1.5f, 0.0f, -5.0f}, Vec3{0.0f, 0.0f, 1.0f}}, sphere));
        assert(!raycast(Ray{Vec3{0.0f, 0.0f, -5.0f}, Vec3{0.0f, 0.0f, -1.0f}}, sphere));
        assert(!raycast(Ray{Vec3{0.0f, 0.0f, -5.0f}, Vec3{0.0f, 0.0f, 0.0f}}, sphere));

        // Too far for the ray's reach; exactly in reach counts.
        assert(!raycast(Ray{Vec3{0.0f, 0.0f, -5.0f}, Vec3{0.0f, 0.0f, 1.0f}}, sphere, 3.9f));
        assert(raycast(Ray{Vec3{0.0f, 0.0f, -5.0f}, Vec3{0.0f, 0.0f, 1.0f}}, sphere, 4.0f));

        // Starting inside: a hit right away, the normal pointing back along the ray.
        assertHit(raycast(Ray{Vec3{0.2f, 0.0f, 0.0f}, Vec3{0.0f, 2.0f, 0.0f}}, sphere),
            0.0f, Vec3{0.2f, 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f});
    }

    void testCapsule() {
        // Upright, 3 tall in all: the segment from y = 0 to y = 2, radius 0.5.
        const Capsule capsule{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 2.0f, 0.0f}, 0.5f};

        // The side, the top from above, and the rounded top end from the side.
        assertHit(raycast(Ray{Vec3{-5.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}, capsule),
            4.5f, Vec3{-0.5f, 1.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f});
        assertHit(raycast(Ray{Vec3{0.0f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}}, capsule),
            2.5f, Vec3{0.0f, 2.5f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
        assertHit(raycast(Ray{Vec3{-5.0f, 2.3f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}, capsule),
            4.6f, Vec3{-0.4f, 2.3f, 0.0f}, Vec3{-0.8f, 0.6f, 0.0f});

        // The bottom end from below.
        assertHit(raycast(Ray{Vec3{0.0f, -4.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}}, capsule),
            3.5f, Vec3{0.0f, -0.5f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f});

        // Parallel to the axis but outside the radius, over the top, and away from it: misses.
        assert(!raycast(Ray{Vec3{0.6f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}}, capsule));
        assert(!raycast(Ray{Vec3{-5.0f, 2.6f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}, capsule));
        assert(!raycast(Ray{Vec3{-5.0f, 1.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f}}, capsule));

        // Out of reach.
        assert(!raycast(Ray{Vec3{-5.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}, capsule, 4.0f));

        // Starting inside.
        assertHit(raycast(Ray{Vec3{0.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}, capsule),
            0.0f, Vec3{0.0f, 1.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f});

        // A capsule whose ends meet is a sphere.
        const Capsule ball{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}, 1.0f};
        assertHit(raycast(Ray{Vec3{0.0f, 0.0f, -5.0f}, Vec3{0.0f, 0.0f, 1.0f}}, ball),
            4.0f, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 0.0f, -1.0f});
    }

    void testBox() {
        const Aabb box{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}};

        // Into the -x face, and down onto the top.
        assertHit(raycast(Ray{Vec3{-5.0f, 0.5f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}, box),
            4.0f, Vec3{-1.0f, 0.5f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f});
        assertHit(raycast(Ray{Vec3{0.5f, 5.0f, 0.2f}, Vec3{0.0f, -1.0f, 0.0f}}, box),
            4.0f, Vec3{0.5f, 1.0f, 0.2f}, Vec3{0.0f, 1.0f, 0.0f});

        // At a slant: enters through the face it reaches last (the +z face here, at t = 3 * sqrt(2)).
        const float slant = std::sqrt(0.5f);
        assertHit(raycast(Ray{Vec3{-3.5f, 0.0f, 4.0f}, Vec3{slant, 0.0f, -slant}}, box),
            3.0f * std::sqrt(2.0f), Vec3{-0.5f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, 1.0f});

        // Parallel to a face but outside it, beside it, and away from it: misses.
        assert(!raycast(Ray{Vec3{-5.0f, 2.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}, box));
        assert(!raycast(Ray{Vec3{-5.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}}, box));
        assert(!raycast(Ray{Vec3{-5.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f}}, box));

        // Out of reach.
        assert(!raycast(Ray{Vec3{-5.0f, 0.5f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}, box, 3.0f));

        // Starting inside.
        assertHit(raycast(Ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}}, box),
            0.0f, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f});
    }

    void testShapeVariant() {
        const Ray ray{Vec3{-5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}};

        assertHit(raycast(ray, Shape{Sphere{Vec3{0.0f, 0.0f, 0.0f}, 2.0f}}),
            3.0f, Vec3{-2.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f});
        assertHit(raycast(ray, Shape{Capsule{Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 2.0f}}),
            3.0f, Vec3{-2.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f});
        assertHit(raycast(ray, Shape{Aabb{Vec3{-2.0f, -1.0f, -1.0f}, Vec3{2.0f, 1.0f, 1.0f}}}),
            3.0f, Vec3{-2.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f});
        assert(!raycast(ray, Shape{Aabb{Vec3{-2.0f, -1.0f, -1.0f}, Vec3{2.0f, 1.0f, 1.0f}}}, 2.0f));
    }

    void testTriangleMesh() {
        // One triangle in the z = 0 plane, counterclockwise seen from +z, and a smaller copy 2 above it.
        TriangleMesh mesh;
        mesh.positions = {
            Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
            Vec3{0.0f, 0.0f, 2.0f}, Vec3{0.5f, 0.0f, 2.0f}, Vec3{0.0f, 0.5f, 2.0f}
        };
        mesh.indices = {0, 1, 2};

        // From the front, and from behind (the normal still faces the ray).
        assertHit(raycast(Ray{Vec3{0.2f, 0.2f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}}, mesh),
            5.0f, Vec3{0.2f, 0.2f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f});
        assertHit(raycast(Ray{Vec3{0.2f, 0.2f, -5.0f}, Vec3{0.0f, 0.0f, 1.0f}}, mesh),
            5.0f, Vec3{0.2f, 0.2f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f});

        // Beside the triangle, along its plane, and pointing away: misses.
        assert(!raycast(Ray{Vec3{0.8f, 0.8f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}}, mesh));
        assert(!raycast(Ray{Vec3{-1.0f, 0.2f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}, mesh));
        assert(!raycast(Ray{Vec3{0.2f, 0.2f, 5.0f}, Vec3{0.0f, 0.0f, 1.0f}}, mesh));

        // With both triangles, the nearest one wins, whatever its order in the list.
        mesh.indices = {0, 1, 2, 3, 4, 5};
        assertHit(raycast(Ray{Vec3{0.2f, 0.2f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}}, mesh),
            3.0f, Vec3{0.2f, 0.2f, 2.0f}, Vec3{0.0f, 0.0f, 1.0f});

        // Outside the upper one, the ray goes on to the lower one.
        assertHit(raycast(Ray{Vec3{0.4f, 0.4f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}}, mesh),
            5.0f, Vec3{0.4f, 0.4f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f});

        // Reach limits it to the upper one or nothing.
        assert(!raycast(Ray{Vec3{0.4f, 0.4f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}}, mesh, 4.0f));

        // An empty mesh is never hit.
        assert(!raycast(Ray{Vec3{0.2f, 0.2f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}}, TriangleMesh{}));
    }
}

void testRaycast() {
    testSphere();
    testCapsule();
    testBox();
    testShapeVariant();
    testTriangleMesh();
}
