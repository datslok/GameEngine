#include "cloth/cloth.h"
#include "cloth/cloth_block_descent.h"
#include "cloth/cloth_energy.h"
#include "cloth/cloth_yardstick.h"
#include "core/job_system.h"
#include "scene/mesh.h"
#include "cloth_helpers.h"
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
        ClothSettings settings = untetheredSettings();
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

    bool sameBlock(const ClothStiffnessBlock& a, const ClothStiffnessBlock& b) {
        return a.xx == b.xx && a.yy == b.yy && a.zz == b.zz && a.xy == b.xy && a.xz == b.xz && a.yz == b.yz;
    }

    /*
    * A point's force and block are sums of terms, one per piece of phi it is in, each added to what is there (the
    * pieces the GPU's per-point pass adds in the same order): the inertia pulls towards the prediction, an edge by
    * Hooke's law along itself (stiff across it only while stretched, a squashed one not at all), an isometric hinge
    * against its curvature, a hinge by its angle along its slope, the hand towards its target.
    */
    void testPointTerms() {
        const float stiffness = 2000.0f;
        Vec3 force{1.0f, 0.0f, 0.0f};
        ClothStiffnessBlock block;
        block.xx = 5.0f;
        addInertiaTerm(force, &block, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.5f, 0.0f}, 100.0f);
        assert(force.x == 1.0f && force.y == -50.0f && force.z == 0.0f);
        assert(block.xx == 105.0f && block.yy == 100.0f && block.zz == 100.0f && block.xy == 0.0f);

        // Stretched along x by 10%: pulled back by k (L - r), stiff k along, k (1 - r / L) across.
        force = Vec3{};
        block = ClothStiffnessBlock{};
        addEdgeTerm(force, &block, Vec3{0.11f, 0.0f, 0.0f}, Vec3{}, 0.1f, stiffness);
        assert(near(force.x, -stiffness * 0.01f, 1e-3f) && force.y == 0.0f && force.z == 0.0f);
        assert(near(block.xx, stiffness, 1e-2f) && near(block.yy, stiffness * (1.0f - 0.1f / 0.11f), 1e-2f) && block.yy == block.zz);
        assert(block.xy == 0.0f && block.xz == 0.0f && block.yz == 0.0f);

        // Squashed: pushed out, stiff only along.
        force = Vec3{};
        block = ClothStiffnessBlock{};
        addEdgeTerm(force, &block, Vec3{0.0f, 0.09f, 0.0f}, Vec3{}, 0.1f, stiffness);
        assert(near(force.y, stiffness * 0.01f, 1e-3f) && block.xx == 0.0f && block.zz == 0.0f && near(block.yy, stiffness, 1e-2f));

        // Too short to have a direction: nothing.
        force = Vec3{};
        block = ClothStiffnessBlock{};
        addEdgeTerm(force, &block, Vec3{}, Vec3{}, 0.1f, stiffness);
        assert(force.x == 0.0f && force.y == 0.0f && force.z == 0.0f && sameBlock(block, ClothStiffnessBlock{}));

        force = Vec3{};
        block = ClothStiffnessBlock{};
        addIsometricHingeTerm(force, &block, Vec3{0.0f, 0.0f, 2.0f}, -0.5f, 4.0f);
        assert(force.z == 4.0f && block.xx == 1.0f && block.yy == 1.0f && block.zz == 1.0f && block.xy == 0.0f);

        force = Vec3{};
        block = ClothStiffnessBlock{};
        addHingeAngleTerm(force, &block, Vec3{1.0f, 2.0f, 0.0f}, 0.25f, 4.0f);
        assert(force.x == -1.0f && force.y == -2.0f && force.z == 0.0f);
        assert(block.xx == 4.0f && block.yy == 16.0f && block.xy == 8.0f && block.zz == 0.0f);

        force = Vec3{};
        block = ClothStiffnessBlock{};
        addHandTerm(force, &block, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 10.0f);
        assert(force.x == -10.0f && force.z == 10.0f && block.xx == 10.0f && block.zz == 10.0f && block.xy == 0.0f);

        // Without a block the force is the same.
        Vec3 alone{};
        addEdgeTerm(alone, nullptr, Vec3{0.11f, 0.0f, 0.0f}, Vec3{}, 0.1f, stiffness);
        Vec3 withBlock{};
        ClothStiffnessBlock unused;
        addEdgeTerm(withBlock, &unused, Vec3{0.11f, 0.0f, 0.0f}, Vec3{}, 0.1f, stiffness);
        assert(alone.x == withBlock.x);
    }

    /*
    * clothPointForce is exactly its terms added in order: the inertia, the point's edges, its hinges (when the fabric
    * bends), the hand. The GPU's pass sums them the same way, so the order is part of what it mirrors. On a busy sheet,
    * every point and both bending models, to the bit.
    */
    void testPointForceIsItsTerms() {
        for (const BendingModel model : {BendingModel::Isometric, BendingModel::Hinge}) {
            ClothSettings settings = blockDescent();
            settings.bendingModel = model;
            Cloth cloth = makeCloth(Mesh::grid(6, 6, 0.3f, 0.3f), Mat4::rotationX(-0.4f), settings);
            addClothPin(cloth, 0);
            cloth.grab = ClothGrab{17, Vec3{0.1f, 0.2f, 0.3f}};
            for (std::size_t point = 0; point < cloth.particles.size(); ++point) {
                cloth.particles.positions[point] = cloth.particles.positions[point] +
                                                   Vec3{0.003f * std::sin(float(point)), 0.002f * std::cos(1.3f * float(point)), 0.001f * float(point % 5)};
            }
            std::vector<Vec3> predicted = cloth.particles.positions;
            for (Vec3& place : predicted) {
                place.y -= 0.0001f;
            }

            const ClothEnergyTerms terms = energyTerms(cloth);
            const float substep = tick / 15.0f;
            const float perSecondSquared = 1.0f / (substep * substep);
            const std::vector<Vec3>& positions = cloth.particles.positions;
            const ClothPointConstraints& lists = cloth.pointConstraints;
            for (std::uint32_t point = 0; point < cloth.particles.size(); ++point) {
                ClothStiffnessBlock expectedBlock;
                const Vec3 expected = clothPointForce(point, cloth.particles, predicted, terms, perSecondSquared, &expectedBlock);
                if (cloth.particles.inverseMasses[point] == 0.0f) {
                    continue;
                }

                Vec3 force{};
                ClothStiffnessBlock block;
                const Vec3 here = positions[point];
                addInertiaTerm(force, &block, here, predicted[point], perSecondSquared / cloth.particles.inverseMasses[point]);
                for (std::uint32_t at = lists.edgeStarts[point]; at < lists.edgeStarts[point + 1]; ++at) {
                    const StretchConstraint& edge = cloth.stretch[lists.edges[at]];
                    addEdgeTerm(force, &block, here, positions[edge.first == point ? edge.second : edge.first], edge.restLength,
                                energyStiffness(settings.stretchCompliance));
                }
                for (std::uint32_t at = lists.hingeStarts[point]; at < lists.hingeStarts[point + 1]; ++at) {
                    const BendConstraint& bend = cloth.bend[lists.hinges[at] / 4];
                    const std::uint32_t corner = lists.hinges[at] % 4;
                    const float stiffness = settings.bendingStiffness * bend.stiffnessShare;
                    const Vec3 error = hingeError(bend, positions, model);
                    if (model == BendingModel::Isometric) {
                        addIsometricHingeTerm(force, &block, error, bend.isometricWeights[corner], stiffness);
                    } else {
                        const Vec3 slope = hingeAngleGradient(positions[bend.wing0], positions[bend.wing1], positions[bend.edgeStart],
                                                              positions[bend.edgeEnd])[corner];
                        addHingeAngleTerm(force, &block, slope, error.x, stiffness);
                    }
                }
                if (point == cloth.grab->particle) {
                    addHandTerm(force, &block, here, cloth.grab->target, energyStiffness(settings.grabCompliance));
                }
                assert(force.x == expected.x && force.y == expected.y && force.z == expected.z);
                assert(sameBlock(block, expectedBlock));
            }
        }
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
    testPointTerms();
    testPointForceIsItsTerms();
}
