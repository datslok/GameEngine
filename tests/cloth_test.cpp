#include "cloth/cloth.h"
#include "cloth/cloth_particles.h"
#include "engine/state_hash.h"
#include "scene/deforming_mesh.h"
#include "scene/mesh.h"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {
    bool near(float actual, float expected, float tolerance = 1e-5f) {
        return std::abs(actual - expected) <= tolerance;
    }

    bool nearVec(const Vec3& actual, const Vec3& expected, float tolerance = 1e-5f) {
        return near(actual.x, expected.x, tolerance) && near(actual.y, expected.y, tolerance) && near(actual.z, expected.z, tolerance);
    }

    float massOf(const ClothParticles& particles, std::size_t index) {
        return 1.0f / particles.inverseMasses[index];
    }

    /*
    * Lumped masses: each point gets a third of every triangle it touches, so the masses add up to the area times the
    * fabric's mass per area. On a sheet of 2 x 2 squares, 1 m each way: a corner on the diagonal touches two triangles,
    * the other two corners one, an edge's middle three, the centre six.
    */
    void testMassesComeFromTheFabric() {
        const Mesh sheet = Mesh::grid(2, 2, 1.0f, 1.0f);
        const ClothParticles particles = makeClothParticles(sheet, 0.2f);
        assert(particles.size() == 9);
        assert(particles.positions.size() == 9 && particles.previousPositions.size() == 9);
        assert(particles.velocities.size() == 9 && particles.inverseMasses.size() == 9);

        const float triangleMass = 0.2f * 0.125f;
        float total = 0.0f;
        for (std::size_t index = 0; index < particles.size(); ++index) {
            total += massOf(particles, index);
            assert(nearVec(particles.positions[index], Vec3{sheet.vertices[index].x, sheet.vertices[index].y, sheet.vertices[index].z}));
            assert(nearVec(particles.velocities[index], Vec3{0.0f, 0.0f, 0.0f}));
        }
        assert(near(total, 0.2f));
        assert(near(massOf(particles, 0), 2.0f * triangleMass / 3.0f));
        assert(near(massOf(particles, 2), triangleMass / 3.0f));
        assert(near(massOf(particles, 4), 6.0f * triangleMass / 3.0f));
    }

    // A point that belongs to no triangle has no mass to give it, and a fabric must weigh something.
    void testRejectsWhatCannotBeCloth() {
        Mesh stray = Mesh::grid(1, 1, 1.0f, 1.0f);
        stray.vertices.push_back(Vec4{5.0f, 0.0f, 0.0f, 1.0f});
        bool threw = false;
        try {
            makeClothParticles(stray, 0.2f);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);

        threw = false;
        try {
            makeClothParticles(Mesh::grid(1, 1, 1.0f, 1.0f), 0.0f);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    /*
    * Free fall: symplectic Euler gains exactly g dt of speed a step, so the velocity worked out from the motion after the
    * step is exactly the predicted one, and the sheet falls at g (not g / 2, as moving by v t + a t^2 / 2 would give).
    */
    void testFallsAtGravity() {
        ClothParticles particles = makeClothParticles(Mesh::grid(1, 1, 1.0f, 1.0f), 0.2f);
        const Vec3 gravity{0.0f, -9.81f, 0.0f};
        const float step = 1.0f / 120.0f;

        for (int tick = 0; tick < 120; ++tick) {
            predictCloth(particles, gravity, step);
            finishClothStep(particles, step);
        }

        for (std::size_t index = 0; index < particles.size(); ++index) {
            assert(near(particles.velocities[index].y, -9.81f, 1e-3f));
            // Symplectic Euler falls g dt^2 (n (n + 1) / 2) in n steps: 4.946 m in a second, against 4.905 exactly.
            const float fallen = particles.previousPositions[index].y - particles.positions[index].y;
            assert(fallen > 0.0f);
        }
        const float expectedDrop = 9.81f * step * step * 120.0f * 121.0f / 2.0f;
        assert(near(Mesh::grid(1, 1, 1.0f, 1.0f).vertices[0].y - particles.positions[0].y, expectedDrop, 1e-3f));
    }

    // Whatever moves the points between the prediction and the end of the step (the constraints, later) sets their
    // velocity: the motion over the step, divided by its length.
    void testVelocityComesFromTheMotion() {
        ClothParticles particles = makeClothParticles(Mesh::grid(1, 1, 1.0f, 1.0f), 0.2f);
        const float step = 0.01f;
        predictCloth(particles, Vec3{0.0f, 0.0f, 0.0f}, step);
        particles.positions[1] = particles.positions[1] + Vec3{0.02f, 0.0f, -0.01f};
        finishClothStep(particles, step);
        assert(nearVec(particles.velocities[1], Vec3{2.0f, 0.0f, -1.0f}, 1e-3f));
        assert(nearVec(particles.velocities[0], Vec3{0.0f, 0.0f, 0.0f}));
    }

    // A pinned point weighs infinitely much (inverse mass 0) and stands still; the rest of the sheet still falls.
    void testPinnedPointsStay() {
        ClothParticles particles = makeClothParticles(Mesh::grid(1, 1, 1.0f, 1.0f), 0.2f);
        particles.velocities[3] = Vec3{1.0f, 0.0f, 0.0f};
        pinParticle(particles, 3);
        assert(particles.inverseMasses[3] == 0.0f);
        assert(nearVec(particles.velocities[3], Vec3{0.0f, 0.0f, 0.0f}));

        const Vec3 start = particles.positions[3];
        for (int tick = 0; tick < 10; ++tick) {
            predictCloth(particles, Vec3{0.0f, -9.81f, 0.0f}, 1.0f / 120.0f);
            finishClothStep(particles, 1.0f / 120.0f);
        }
        assert(particles.positions[3].x == start.x && particles.positions[3].y == start.y && particles.positions[3].z == start.z);
        assert(particles.positions[0].y < Mesh::grid(1, 1, 1.0f, 1.0f).vertices[0].y);

        bool threw = false;
        try {
            pinParticle(particles, 4);
        } catch (const std::out_of_range&) {
            threw = true;
        }
        assert(threw);
    }

    // A cloth entity: placed in the world, stepped each tick, its points handed to its deforming mesh to be drawn.
    void testClothDrivesItsMesh() {
        World world;
        const Mesh sheet = Mesh::grid(2, 1, 2.0f, 1.0f);
        const Mat4 placement = Mat4::translation(0.0f, 3.0f, 0.0f);
        Cloth cloth = makeCloth(sheet, placement, ClothSettings{});
        assert(nearVec(cloth.particles.positions[0], Vec3{-1.0f, 2.5f, 0.0f}));

        const Entity entity = world.create();
        world.add(entity, std::move(cloth));
        DeformingMesh mesh;
        mesh.mesh = MeshHandle{0};
        world.add(entity, mesh);

        stepCloths(world, 1.0f / 120.0f);
        const Cloth& stepped = world.get<Cloth>(entity);
        const DeformingMesh& shown = world.get<DeformingMesh>(entity);
        assert(stepped.particles.positions[0].y < 2.5f);
        assert(shown.positions.size() == stepped.particles.size());
        for (std::size_t index = 0; index < shown.positions.size(); ++index) {
            assert(nearVec(shown.positions[index], stepped.particles.positions[index]));
        }

        // Its points are part of the simulation's state.
        const std::uint64_t before = hashSimulationState(world);
        world.get<Cloth>(entity).particles.velocities[2].x += 1.0f;
        assert(hashSimulationState(world) != before);
    }

    // The defaults: a light cotton (200 g/m2, about 2000 N/m stiff) under real gravity, 30 substeps a tick, bending in every one.
    void testDefaults() {
        const ClothSettings settings;
        assert(near(settings.areaDensity, 0.2f));
        assert(near(settings.gravity, 9.81f));
        assert(near(settings.stretchCompliance, 1.0f / 2000.0f));
        assert(settings.substeps == 30);
        assert(settings.bendingInterval == 1);
        assert(near(settings.drag, 0.5f));
        assert(near(settings.grabCompliance, 0.001f));
        assert(near(settings.bendingStiffness, 1e-5f));
    }
}

void testCloth() {
    testMassesComeFromTheFabric();
    testRejectsWhatCannotBeCloth();
    testFallsAtGravity();
    testVelocityComesFromTheMotion();
    testPinnedPointsStay();
    testClothDrivesItsMesh();
    testDefaults();
}
