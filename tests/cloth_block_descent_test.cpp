#include "cloth/cloth.h"
#include "cloth/cloth_block_descent.h"
#include "cloth/cloth_energy.h"
#include "cloth/cloth_yardstick.h"
#include "core/job_system.h"
#include "scene/mesh.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <optional>
#include <set>
#include <vector>

namespace {
    constexpr float tick = 1.0f / 120.0f;
    constexpr float pi = std::numbers::pi_v<float>;

    ClothSettings blockDescent() {
        ClothSettings settings;
        settings.method = ClothMethod::VertexBlockDescent;
        return settings;
    }

    ClothEnergyTerms energyTerms(const Cloth& cloth) {
        const ClothSettings& settings = cloth.settings;
        return ClothEnergyTerms{cloth.stretch,           cloth.bend,               cloth.pointConstraints, settings.stretchCompliance,
                                settings.bendingStiffness, settings.bendingModel, cloth.grab,            settings.grabCompliance};
    }

    /*
    * Every point is in exactly one colour, and no two points of a colour share an edge or a hinge, so each colour's
    * points can take their steps at once; a grid needs a couple of dozen colours at most.
    */
    void testPointsAreColoured() {
        const Cloth cloth = makeCloth(Mesh::grid(10, 8, 1.0f, 1.0f), Mat4::identity(), ClothSettings{});
        const ClothVertexColours& blocks = cloth.vertexColours;
        const ClothPointConstraints& lists = cloth.pointConstraints;
        assert(blocks.order.size() == cloth.particles.size());
        assert(std::set<std::uint32_t>(blocks.order.begin(), blocks.order.end()).size() == cloth.particles.size());
        assert(blocks.colours.front() == 0 && blocks.colours.back() == blocks.order.size());
        assert(blocks.colours.size() - 1 <= 24);

        std::vector<int> colourOf(cloth.particles.size());
        for (std::size_t colour = 0; colour + 1 < blocks.colours.size(); ++colour) {
            for (std::uint32_t at = blocks.colours[colour]; at < blocks.colours[colour + 1]; ++at) {
                colourOf[blocks.order[at]] = static_cast<int>(colour);
            }
        }
        for (const StretchConstraint& edge : cloth.stretch) {
            assert(colourOf[edge.first] != colourOf[edge.second]);
        }
        for (const BendConstraint& hinge : cloth.bend) {
            const std::uint32_t points[4]{hinge.wing0, hinge.wing1, hinge.edgeStart, hinge.edgeEnd};
            for (int a = 0; a < 4; ++a) {
                for (int b = a + 1; b < 4; ++b) {
                    assert(colourOf[points[a]] != colourOf[points[b]]);
                }
            }
        }

        // Each point lists exactly the edges and hinge corners it is in.
        for (std::uint32_t point = 0; point < cloth.particles.size(); ++point) {
            for (std::uint32_t at = lists.edgeStarts[point]; at < lists.edgeStarts[point + 1]; ++at) {
                const StretchConstraint& edge = cloth.stretch[lists.edges[at]];
                assert(edge.first == point || edge.second == point);
            }
            for (std::uint32_t at = lists.hingeStarts[point]; at < lists.hingeStarts[point + 1]; ++at) {
                const BendConstraint& hinge = cloth.bend[lists.hinges[at] / 4];
                const std::uint32_t points[4]{hinge.wing0, hinge.wing1, hinge.edgeStart, hinge.edgeEnd};
                assert(points[lists.hinges[at] % 4] == point);
            }
        }
        assert(lists.edges.size() == 2 * cloth.stretch.size() && lists.hinges.size() == 4 * cloth.bend.size());
    }

    /*
    * A sheet with nothing holding it falls freely, as XPBD's does: its edges stay at rest, so every sweep leaves the
    * prediction all but alone. Over 1,800 substeps the float positions round each substep's 3 micrometres of gravity
    * 5 m from the origin, so both solvers end about 0.1% short of g (measured: vertex block descent 9.797 m/s, XPBD
    * 9.799); allowed 0.3%.
    */
    void testFreeFall() {
        const auto fall = [](const ClothSettings& settings) {
            Cloth cloth = makeCloth(Mesh::grid(6, 6, 1.0f, 1.0f), Mat4::identity(), settings);
            cloth.settings.drag = 0.0f;
            for (int step = 0; step < 120; ++step) {
                stepCloth(cloth, tick);
            }
            return cloth.particles.velocities[24];
        };
        const Vec3 velocity = fall(blockDescent());
        const float gravity = ClothSettings{}.gravity;
        assert(near(velocity.y, -gravity, 0.003f * gravity) && near(velocity.x, 0.0f, 0.003f) && near(velocity.z, 0.0f, 0.003f));
        assert(near(velocity.y, fall(ClothSettings{}).y, 0.001f * gravity));
    }

    /*
    * Hooke's law, the reason for real units: a 0.5 kg weight hanging from a pinned point by an edge of compliance
    * alpha settles stretched by m g alpha, the implicit step's own answer. Settled with damping, within 1%.
    */
    void testHangingWeightStretchesByHookesLaw() {
        const float mass = 0.5f;
        const float compliance = 0.002f;
        const float gravity = 9.81f;
        ClothParticles particles;
        particles.positions = {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}};
        particles.previousPositions = particles.positions;
        particles.velocities.assign(2, Vec3{0.0f, 0.0f, 0.0f});
        particles.inverseMasses = {0.0f, 1.0f / mass};
        const std::vector<StretchConstraint> stretch{{0, 1, 1.0f}};
        const std::vector<BendConstraint> bend;
        const std::optional<ClothGrab> grab;
        const ClothPointConstraints lists = clothPointConstraints(stretch, bend, 2);
        const ClothEnergyTerms terms{stretch, bend, lists, compliance, 0.0f, BendingModel::Isometric, grab, 0.0f};
        const ClothVertexColours colours = clothVertexColours(stretch, bend, lists);

        const float substep = 1.0f / 1800.0f;
        for (int step = 0; step < 20000; ++step) {
            predictCloth(particles, Vec3{0.0f, -gravity, 0.0f}, substep);
            const std::vector<Vec3> predicted = particles.positions;
            solveVertexBlocks(particles, predicted, colours, terms, substep, serialJobs());
            finishClothStep(particles, substep);
            particles.velocities[1] = particles.velocities[1] * 0.995f;
        }
        const float stretched = -particles.positions[1].y - 1.0f;
        assert(near(stretched, mass * gravity * compliance, 0.01f * mass * gravity * compliance));
    }

    // A sweep from the prediction never raises the substep's energy: over a busy second (moving pins, a hand, a ball).
    void testSweepLowersEnergy() {
        Cloth cloth = makeCloth(Mesh::grid(30, 30, 1.5f, 1.5f), Mat4::translation(0.0f, 1.0f, 0.0f) * Mat4::rotationX(-0.5f * pi), blockDescent());
        addClothPin(cloth, 30 * 31);
        addClothPin(cloth, 30 * 31 + 30);
        cloth.grab = ClothGrab{15, Vec3{0.3f, 0.8f, 0.6f}};
        const std::vector<Shape> ball{Shape{Sphere{Vec3{0.2f, 0.3f, 0.3f}, 0.35f}}};
        const float substep = tick / static_cast<float>(cloth.settings.substeps);
        for (int step = 0; step < 120 * cloth.settings.substeps; ++step) {
            predictCloth(cloth.particles, Vec3{0.0f, -cloth.settings.gravity, 0.0f}, substep);
            const std::vector<Vec3> predicted = cloth.particles.positions;
            const double before = clothStepEnergy(cloth.particles, predicted, energyTerms(cloth), substep);
            solveVertexBlocks(cloth.particles, predicted, cloth.vertexColours, energyTerms(cloth), substep, serialJobs());
            assert(clothStepEnergy(cloth.particles, predicted, energyTerms(cloth), substep) <= before);
            solveContacts(cloth.particles, ball, cloth.settings);
            finishClothStep(cloth.particles, substep);
        }
    }

    // A sheet hung from its top corners settles, hanging about its height (vertex block descent alone, without levels,
    // stretches as XPBD does: the levels are what carry the pins' pull across it).
    void testHangingSheetSettles() {
        Cloth cloth = yardstickSheet(16, blockDescent());
        const float motion = settleYardstick(cloth, 4.0f, tick);
        assert(motion < 0.01f);
        assert(near(measureSheet(cloth, 16).sag, 1.0f, 0.05f));
    }

    // On one thread or eight, a busy sheet comes out the same to the last bit: within a colour the order cannot matter.
    void testManyThreadsMatchOne() {
        const auto run = [](JobSystem& jobs) {
            Cloth cloth = makeCloth(Mesh::grid(40, 40, 1.5f, 1.5f), Mat4::translation(0.0f, 1.0f, 0.0f) * Mat4::rotationX(-0.5f * pi), blockDescent());
            addClothPin(cloth, 40 * 41);
            addClothPin(cloth, 40 * 41 + 40);
            const std::vector<Shape> ball{Shape{Sphere{Vec3{0.2f, 0.3f, 0.3f}, 0.35f}}};
            for (int step = 0; step < 60; ++step) {
                cloth.grab = ClothGrab{20, Vec3{0.3f, 0.8f + 0.001f * step, 0.6f}};
                stepCloth(cloth, tick, ball, jobs);
            }
            return cloth.particles.positions;
        };
        JobSystem serial{0};
        JobSystem parallel{7};
        const std::vector<Vec3> one = run(serial);
        const std::vector<Vec3> many = run(parallel);
        assert(std::memcmp(one.data(), many.data(), one.size() * sizeof(Vec3)) == 0);
    }

    // The hinge model works the same way: a busy sheet bending by angle steps without blowing up, its energy lowered too.
    void testHingeModel() {
        ClothSettings settings = blockDescent();
        settings.bendingModel = BendingModel::Hinge;
        settings.bendingStiffness = 1e-4f;
        Cloth cloth = yardstickSheet(12, settings);
        const float motion = settleYardstick(cloth, 3.0f, tick);
        assert(std::isfinite(motion) && motion < 0.02f);
        assert(near(measureSheet(cloth, 12).sag, 1.0f, 0.05f));
    }
}

void testClothBlockDescent() {
    testPointsAreColoured();
    testFreeFall();
    testHangingWeightStretchesByHookesLaw();
    testSweepLowersEnergy();
    testHangingSheetSettles();
    testManyThreadsMatchOne();
    testHingeModel();
}
