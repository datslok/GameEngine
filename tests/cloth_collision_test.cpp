#include "cloth/cloth.h"
#include "cloth/cloth_collision.h"
#include "physics/collider.h"
#include "math/transform.h"
#include "scene/mesh.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <vector>

namespace {
    constexpr float pi = std::numbers::pi_v<float>;

    bool near(float actual, float expected, float tolerance) {
        return std::abs(actual - expected) <= tolerance;
    }

    bool nearVec(const Vec3& actual, const Vec3& expected, float tolerance) {
        return near(actual.x, expected.x, tolerance) && near(actual.y, expected.y, tolerance) && near(actual.z, expected.z, tolerance);
    }

    // One free point, at rest, weighing 10 g.
    ClothParticles onePoint(const Vec3& position) {
        ClothParticles particles;
        particles.positions = {position};
        particles.previousPositions = {position};
        particles.velocities = {Vec3{0.0f, 0.0f, 0.0f}};
        particles.inverseMasses = {100.0f};
        return particles;
    }

    // How far a point is from each shape's surface (negative inside) and the way out, the normal at the closest point.
    void testSignedDistances() {
        const Sphere ball{Vec3{0.0f, 1.0f, 0.0f}, 0.5f};
        SurfacePoint surface = surfacePoint(Shape{ball}, Vec3{0.0f, 2.0f, 0.0f});
        assert(near(surface.distance, 0.5f, 1e-6f) && nearVec(surface.normal, Vec3{0.0f, 1.0f, 0.0f}, 1e-6f));
        surface = surfacePoint(Shape{ball}, Vec3{0.2f, 1.0f, 0.0f});
        assert(near(surface.distance, -0.3f, 1e-6f) && nearVec(surface.normal, Vec3{1.0f, 0.0f, 0.0f}, 1e-6f));
        // At the very centre any way out will do, as long as it is one.
        assert(near(surfacePoint(Shape{ball}, ball.centre).normal.length(), 1.0f, 1e-6f));

        const Capsule pill{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 2.0f, 0.0f}, 0.25f};
        surface = surfacePoint(Shape{pill}, Vec3{0.0f, 1.0f, 1.0f});
        assert(near(surface.distance, 0.75f, 1e-6f) && nearVec(surface.normal, Vec3{0.0f, 0.0f, 1.0f}, 1e-6f));
        surface = surfacePoint(Shape{pill}, Vec3{0.0f, 2.1f, 0.0f});
        assert(near(surface.distance, -0.15f, 1e-6f) && nearVec(surface.normal, Vec3{0.0f, 1.0f, 0.0f}, 1e-6f));

        const Aabb box{Vec3{-1.0f, 0.0f, -1.0f}, Vec3{1.0f, 0.5f, 1.0f}};
        surface = surfacePoint(Shape{box}, Vec3{0.3f, 0.8f, 0.2f});
        assert(near(surface.distance, 0.3f, 1e-6f) && nearVec(surface.normal, Vec3{0.0f, 1.0f, 0.0f}, 1e-6f));
        surface = surfacePoint(Shape{box}, Vec3{2.0f, 1.5f, 0.0f});
        assert(near(surface.distance, std::sqrt(2.0f), 1e-5f) && nearVec(surface.normal, Vec3{1.0f, 1.0f, 0.0f}.normalized(), 1e-5f));
        // Inside, out through the nearest face: 0.1 below the top, 0.9 from the sides.
        surface = surfacePoint(Shape{box}, Vec3{0.1f, 0.4f, 0.0f});
        assert(near(surface.distance, -0.1f, 1e-6f) && nearVec(surface.normal, Vec3{0.0f, 1.0f, 0.0f}, 1e-6f));
    }

    // A point inside a shape, or closer than the cloth's thickness, is pushed out along the normal to the thickness;
    // one well clear is left alone, and so is a pinned one.
    void testPointsArePushedOut() {
        ClothSettings settings;
        const std::vector<Shape> shapes{Shape{Sphere{Vec3{0.0f, 0.0f, 0.0f}, 1.0f}}};

        ClothParticles inside = onePoint(Vec3{0.0f, 0.9f, 0.0f});
        solveContacts(inside, shapes, settings);
        assert(nearVec(inside.positions[0], Vec3{0.0f, 1.0f + settings.thickness, 0.0f}, 1e-6f));

        ClothParticles clear = onePoint(Vec3{0.0f, 1.5f, 0.0f});
        solveContacts(clear, shapes, settings);
        assert(nearVec(clear.positions[0], Vec3{0.0f, 1.5f, 0.0f}, 0.0f));

        ClothParticles pinned = onePoint(Vec3{0.0f, 0.5f, 0.0f});
        pinned.inverseMasses[0] = 0.0f;
        solveContacts(pinned, shapes, settings);
        assert(nearVec(pinned.positions[0], Vec3{0.0f, 0.5f, 0.0f}, 0.0f));
    }

    /*
    * Friction on the positions, as Coulomb's law: a slide along the surface shorter than the static friction times how
    * deep the contact pushed is undone (the point sticks); a longer one is shortened by the kinetic friction times the
    * push; with no friction it is kept.
    */
    void testFriction() {
        ClothSettings settings;
        const std::vector<Shape> floor{Shape{Aabb{Vec3{-10.0f, -1.0f, -10.0f}, Vec3{10.0f, 0.0f, 10.0f}}}};

        // Started on the surface, moved 1 mm sideways and 2 mm down into it this substep: the push is 2 mm plus the
        // thickness, and the slide is well under the static friction times it, so it sticks.
        ClothParticles sticking = onePoint(Vec3{0.0f, settings.thickness, 0.0f});
        sticking.positions[0] = Vec3{0.001f, settings.thickness - 0.002f, 0.0f};
        solveContacts(sticking, floor, settings);
        assert(nearVec(sticking.positions[0], Vec3{0.0f, settings.thickness, 0.0f}, 1e-6f));

        // A 5 cm slide with the same push slips, shortened by the kinetic friction times the push.
        ClothParticles slipping = onePoint(Vec3{0.0f, settings.thickness, 0.0f});
        slipping.positions[0] = Vec3{0.05f, settings.thickness - 0.002f, 0.0f};
        solveContacts(slipping, floor, settings);
        assert(near(slipping.positions[0].x, 0.05f - settings.kineticFriction * 0.002f, 1e-6f));
        assert(near(slipping.positions[0].y, settings.thickness, 1e-6f));

        ClothSettings slippery = settings;
        slippery.staticFriction = 0.0f;
        slippery.kineticFriction = 0.0f;
        ClothParticles free = onePoint(Vec3{0.0f, settings.thickness, 0.0f});
        free.positions[0] = Vec3{0.001f, settings.thickness - 0.002f, 0.0f};
        solveContacts(free, floor, slippery);
        assert(near(free.positions[0].x, 0.001f, 1e-6f));

        // A point being sewn is pushed out but keeps its slide, as with no friction; the one beside it sticks.
        ClothParticles sewn = onePoint(Vec3{0.0f, settings.thickness, 0.0f});
        sewn.positions.push_back(Vec3{1.0f, settings.thickness - 0.002f, 0.0f});
        sewn.previousPositions.push_back(Vec3{0.999f, settings.thickness, 0.0f});
        sewn.velocities.push_back(Vec3{0.0f, 0.0f, 0.0f});
        sewn.inverseMasses.push_back(100.0f);
        sewn.positions[0] = Vec3{0.001f, settings.thickness - 0.002f, 0.0f};
        const std::vector<std::uint8_t> sewing{1, 0};
        solveContacts(sewn, floor, settings, {}, sewing);
        assert(nearVec(sewn.positions[0], Vec3{0.001f, settings.thickness, 0.0f}, 1e-6f));
        assert(nearVec(sewn.positions[1], Vec3{0.999f, settings.thickness, 0.0f}, 1e-6f));
    }

    // A point resting on a ball stays where the slope is gentler than its friction angle (atan 0.6, 31 degrees) and
    // slides off where it is steeper.
    void testRestingOnASlope() {
        ClothSettings settings;
        settings.drag = 0.0f;
        const Sphere ball{Vec3{0.0f, 0.0f, 0.0f}, 1.0f};
        const std::vector<Shape> shapes{Shape{ball}};
        const auto placedAt = [&](float degreesFromTop) {
            const float angle = degreesFromTop * pi / 180.0f;
            return Vec3{std::sin(angle), std::cos(angle), 0.0f} * (ball.radius + settings.thickness);
        };

        for (float degrees : {10.0f, 20.0f}) {
            ClothParticles point = onePoint(placedAt(degrees));
            for (int tick = 0; tick < 120; ++tick) {
                stepClothParticles(point, {}, settings, 1.0f / 120.0f, std::nullopt, shapes);
            }
            assert((point.positions[0] - placedAt(degrees)).length() < 1e-3f);
        }

        ClothParticles steep = onePoint(placedAt(50.0f));
        for (int tick = 0; tick < 60; ++tick) {
            stepClothParticles(steep, {}, settings, 1.0f / 120.0f, std::nullopt, shapes);
        }
        assert((steep.positions[0] - placedAt(50.0f)).length() > 0.05f);
    }

    /*
    * A sheet dropped on a ball drapes over it: no point ends up inside the ball, it comes to rest, and the same run
    * twice gives the same bits.
    */
    void testSheetDrapesOverABall() {
        const Sphere ball{Vec3{0.0f, 0.0f, 0.0f}, 0.4f};
        const std::vector<Shape> shapes{Shape{ball}};
        const auto drop = [&] {
            Cloth cloth = makeCloth(Mesh::grid(16, 16, 1.0f, 1.0f), Mat4::translation(0.0f, 0.6f, 0.0f) * Mat4::rotationX(-0.5f * pi), ClothSettings{});
            cloth.settings.drag = 2.0f;
            for (int tick = 0; tick < 480; ++tick) {
                stepCloth(cloth, 1.0f / 120.0f, shapes);
            }
            return cloth;
        };

        const Cloth cloth = drop();
        float fastest = 0.0f;
        for (std::size_t index = 0; index < cloth.particles.size(); ++index) {
            assert(surfacePoint(Shape{ball}, cloth.particles.positions[index]).distance > 0.5f * cloth.settings.thickness);
            fastest = std::max(fastest, cloth.particles.velocities[index].length());
        }
        // Its middle rests on top of the ball.
        assert(near(cloth.particles.positions[8 * 17 + 8].y, ball.radius + cloth.settings.thickness, 0.01f));
        assert(fastest < 0.05f);

        const Cloth again = drop();
        assert(std::memcmp(again.particles.positions.data(), cloth.particles.positions.data(), cloth.particles.size() * sizeof(Vec3)) == 0);
    }

    // The world's colliders, placed by their entities, are what every cloth collides with.
    void testClothsCollideWithTheWorldsColliders() {
        World world;
        const Entity ball = world.create();
        Transform placement;
        placement.position = Vec3{0.0f, -1.0f, 0.0f};
        world.add(ball, placement);
        world.add(ball, Collider{Shape{Sphere{Vec3{0.0f, 0.0f, 0.0f}, 0.5f}}, nullptr});

        const std::vector<Shape> shapes = worldColliderShapes(world);
        assert(shapes.size() == 1);
        assert(nearVec(std::get<Sphere>(shapes[0]).centre, Vec3{0.0f, -1.0f, 0.0f}, 1e-6f));

        const Entity sheet = world.create();
        Cloth cloth = makeCloth(Mesh::grid(4, 4, 0.4f, 0.4f), Mat4::translation(0.0f, -0.4f, 0.0f) * Mat4::rotationX(-0.5f * pi), ClothSettings{});
        world.add(sheet, std::move(cloth));
        for (int tick = 0; tick < 120; ++tick) {
            stepCloths(world, 1.0f / 120.0f);
        }
        for (const Vec3& point : world.get<Cloth>(sheet).particles.positions) {
            assert(surfacePoint(shapes[0], point).distance > 0.0f);
        }
    }

    // The defaults: a 5 mm gap, cotton's friction on a smooth surface.
    void testDefaults() {
        const ClothSettings settings;
        assert(near(settings.thickness, 0.005f, 1e-9f));
        assert(near(settings.staticFriction, 0.6f, 1e-9f));
        assert(near(settings.kineticFriction, 0.4f, 1e-9f));
    }
}

void testClothCollision() {
    testSignedDistances();
    testPointsArePushedOut();
    testFriction();
    testRestingOnASlope();
    testSheetDrapesOverABall();
    testClothsCollideWithTheWorldsColliders();
    testDefaults();
}
