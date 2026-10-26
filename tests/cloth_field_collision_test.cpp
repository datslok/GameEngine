#include "cloth/cloth.h"
#include "cloth/cloth_collision.h"
#include "math/mat4.h"
#include "physics/signed_distance_field.h"
#include "scene/collision_mesh.h"
#include "scene/mesh.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <vector>

namespace {
    constexpr float pi = std::numbers::pi_v<float>;
    constexpr float tick = 1.0f / 120.0f;

    const Vec3 ballCentre{0.0f, 0.0f, 0.0f};
    constexpr float ballRadius = 0.4f;

    // A ball given as a distance field: a finely divided sphere mesh, 1 cm samples, a 3 cm band.
    std::shared_ptr<const SignedDistanceField> ballField() {
        static const std::shared_ptr<const SignedDistanceField> field = std::make_shared<const SignedDistanceField>(buildSignedDistanceField(
            collisionMesh(Mesh::sphere(64, 32), Mat4::translation(ballCentre.x, ballCentre.y, ballCentre.z) * Mat4::scaling(ballRadius, ballRadius, ballRadius)),
            0.01f, 0.03f));
        return field;
    }

    ClothParticles onePoint(const Vec3& position) {
        ClothParticles particles;
        particles.positions = {position};
        particles.previousPositions = {position};
        particles.velocities = {Vec3{0.0f, 0.0f, 0.0f}};
        particles.inverseMasses = {100.0f};
        return particles;
    }

    // A field body answers as a shape does: how far, and the unit way out; outside its box, a band away, out being up.
    void testFieldSurfacePoint() {
        const SignedDistanceField& field = *ballField();
        const Vec3 out = Vec3{0.3f, 0.8f, -0.2f}.normalized();
        const SurfacePoint surface = surfacePoint(field, ballCentre + out * (ballRadius + 0.006f));
        assert(near(surface.distance, 0.006f, 0.002f));
        assert(near(surface.normal.length(), 1.0f, 1e-5f) && surface.normal.dot(out) > 0.99f);

        const SurfacePoint far = surfacePoint(field, Vec3{3.0f, 0.0f, 0.0f});
        assert(far.distance == field.band);
        assert(far.normal.x == 0.0f && far.normal.y == 1.0f && far.normal.z == 0.0f);
    }

    // A point inside a field body is pushed out along its normal to the cloth's thickness, as from a shape.
    void testPointsArePushedOutOfFields() {
        const ClothSettings settings;
        const Vec3 out = Vec3{-0.5f, 0.6f, 0.4f}.normalized();
        ClothParticles particles = onePoint(ballCentre + out * (ballRadius - 0.01f));
        particles.previousPositions[0] = particles.positions[0];
        solveContacts(particles, {}, settings, {ballField()});
        const Vec3 moved = particles.positions[0] - ballCentre;
        assert(near(moved.length(), ballRadius + settings.thickness, 0.002f));
        assert(moved.normalized().dot(out) > 0.999f);

        // Pinned points stay, and points clear of the body are left alone.
        ClothParticles pinned = onePoint(ballCentre + out * (ballRadius - 0.01f));
        pinned.inverseMasses[0] = 0.0f;
        const Vec3 before = pinned.positions[0];
        solveContacts(pinned, {}, settings, {ballField()});
        assert(std::memcmp(&pinned.positions[0], &before, sizeof(Vec3)) == 0);

        ClothParticles clear = onePoint(ballCentre + out * (ballRadius + 0.02f));
        const Vec3 clearBefore = clear.positions[0];
        solveContacts(clear, {}, settings, {ballField()});
        assert(std::memcmp(&clear.positions[0], &clearBefore, sizeof(Vec3)) == 0);
    }

    /*
    * A sheet dropped onto the ball as a field drapes over it as over the ball as a sphere: no point inside, its middle
    * resting on top within the mesh's facets and the samples' blend, settled; the same run twice gives the same bits.
    */
    void testSheetDrapesOverAFieldBall() {
        const auto drop = [] {
            Cloth cloth = makeCloth(Mesh::grid(16, 16, 1.0f, 1.0f), Mat4::translation(0.0f, 0.6f, 0.0f) * Mat4::rotationX(-0.5f * pi), ClothSettings{});
            cloth.settings.drag = 2.0f;
            for (int step = 0; step < 480; ++step) {
                stepCloth(cloth, tick, {}, serialJobs(), {ballField()});
            }
            return cloth;
        };

        const Cloth cloth = drop();
        float fastest = 0.0f;
        for (std::size_t index = 0; index < cloth.particles.size(); ++index) {
            assert(surfacePoint(*ballField(), cloth.particles.positions[index]).distance > 0.5f * cloth.settings.thickness);
            fastest = std::max(fastest, cloth.particles.velocities[index].length());
        }
        assert(near(cloth.particles.positions[8 * 17 + 8].y, ballRadius + cloth.settings.thickness, 0.01f));
        assert(fastest < 0.05f);

        const Cloth again = drop();
        assert(std::memcmp(again.particles.positions.data(), cloth.particles.positions.data(), cloth.particles.size() * sizeof(Vec3)) == 0);
    }

    // The world's field bodies are what every cloth collides with too, and a tick queued for the GPU carries them.
    void testClothsCollideWithTheWorldsFields() {
        World world;
        world.add(world.create(), DistanceFieldCollider{ballField()});
        const ClothFields fields = worldDistanceFields(world);
        assert(fields.size() == 1 && fields[0] == ballField());

        const Entity sheet = world.create();
        world.add(sheet, makeCloth(Mesh::grid(8, 8, 0.6f, 0.6f), Mat4::translation(0.0f, 0.45f, 0.0f) * Mat4::rotationX(-0.5f * pi), ClothSettings{}));
        for (int step = 0; step < 120; ++step) {
            stepCloths(world, tick);
        }
        for (const Vec3& point : world.get<Cloth>(sheet).particles.positions) {
            assert(surfacePoint(*ballField(), point).distance > 0.0f);
        }

        stepCloths(world, tick, serialJobs(), ClothSolver::Gpu);
        const Cloth& queued = world.get<Cloth>(sheet);
        assert(queued.gpuTicks.size() == 1 && queued.gpuTicks[0].fields.size() == 1 && queued.gpuTicks[0].fields[0] == ballField());
    }
}

void testClothFieldCollision() {
    testFieldSurfacePoint();
    testPointsArePushedOutOfFields();
    testSheetDrapesOverAFieldBall();
    testClothsCollideWithTheWorldsFields();
}
