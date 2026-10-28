#include "cloth/cloth.h"
#include "cloth/cloth_constraints.h"
#include "core/job_system.h"
#include "ecs/world.h"
#include "scene/mesh.h"

#include <cassert>
#include <cstring>
#include <numbers>
#include <set>
#include <vector>

namespace {
    constexpr float pi = std::numbers::pi_v<float>;

    // Each colour's constraints share no point, the colours cover every constraint once, in order, from 0 to the end.
    template <typename Constraint, typename PointsOf>
    void checkColours(const std::vector<Constraint>& constraints, const std::vector<std::uint32_t>& colours, PointsOf pointsOf) {
        assert(colours.size() >= 2);
        assert(colours.front() == 0 && colours.back() == constraints.size());
        for (std::size_t colour = 0; colour + 1 < colours.size(); ++colour) {
            assert(colours[colour] < colours[colour + 1]);
            std::set<std::uint32_t> used;
            for (std::uint32_t index = colours[colour]; index < colours[colour + 1]; ++index) {
                for (std::uint32_t point : pointsOf(constraints[index])) {
                    assert(used.insert(point).second);
                }
            }
        }
    }

    /*
    * A cloth's constraints are sorted into colours, groups in which no two share a point, so each group can be solved on
    * many threads at once. A grid needs few: its threads and diagonals six, as many as meet at a point inside it (fewer
    * is impossible), its hinges 13 (DSatur; greedy in the mesh's order gave up to 16).
    */
    void testConstraintsAreColoured() {
        const Cloth cloth = makeCloth(Mesh::grid(10, 8, 1.0f, 1.0f), Mat4::identity(), ClothSettings{});
        assert(cloth.stretch.size() == stretchConstraints(Mesh::grid(10, 8, 1.0f, 1.0f)).size());
        assert(cloth.bend.size() == bendConstraints(Mesh::grid(10, 8, 1.0f, 1.0f)).size());

        checkColours(cloth.stretch, cloth.stretchColours, [](const StretchConstraint& edge) {
            return std::vector<std::uint32_t>{edge.first, edge.second};
        });
        checkColours(cloth.bend, cloth.bendColours, [](const BendConstraint& hinge) {
            return std::vector<std::uint32_t>{hinge.wing0, hinge.wing1, hinge.edgeStart, hinge.edgeEnd};
        });
        assert(cloth.stretchColours.size() - 1 == 6);
        assert(cloth.bendColours.size() - 1 <= 13);
    }

    // A hanging sheet with a hand, moving pins and a ball to land on: everything a tick solves.
    Cloth busySheet(BendingModel model) {
        ClothSettings settings;
        settings.bendingModel = model;
        Cloth cloth = makeCloth(Mesh::grid(40, 40, 1.5f, 1.5f), Mat4::translation(0.0f, 1.0f, 0.0f) * Mat4::rotationX(-0.5f * pi), settings);
        addClothPin(cloth, 40 * 41);
        addClothPin(cloth, 40 * 41 + 40);
        return cloth;
    }

    void stepBusy(Cloth& cloth, int tick, JobSystem& jobs) {
        const std::vector<Shape> ball{Shape{Sphere{Vec3{0.2f, 0.3f, 0.3f}, 0.35f}}};
        cloth.pins[1].position = cloth.pins[1].position + Vec3{-0.002f, 0.0f, 0.0f};
        cloth.grab = ClothGrab{20, Vec3{0.3f, 0.8f + 0.001f * tick, 0.6f}};
        stepCloth(cloth, 1.0f / 120.0f, ball, jobs);
    }

    // On one thread or eight, a cloth comes out the same to the last bit with either bending: within a colour, the order
    // cannot matter.
    void testManyThreadsMatchOne() {
        JobSystem serial{0};
        JobSystem parallel{7};
        for (const BendingModel model : {BendingModel::Isometric, BendingModel::Hinge}) {
            Cloth one = busySheet(model);
            Cloth many = busySheet(model);
            for (int tick = 0; tick < 60; ++tick) {
                stepBusy(one, tick, serial);
                stepBusy(many, tick, parallel);
            }
            assert(std::memcmp(one.particles.positions.data(), many.particles.positions.data(), one.particles.size() * sizeof(Vec3)) == 0);
            assert(std::memcmp(one.particles.velocities.data(), many.particles.velocities.data(), one.particles.size() * sizeof(Vec3)) == 0);
        }
    }

    // Several cloths stepped at once, a job each, come out as they do one after another.
    void testClothsStepTogether() {
        const auto run = [](JobSystem& jobs) {
            World world;
            std::vector<Entity> sheets;
            for (int sheet = 0; sheet < 3; ++sheet) {
                const Entity entity = world.create();
                world.add(entity, makeCloth(Mesh::grid(12, 12, 1.0f, 1.0f),
                                            Mat4::translation(2.0f * sheet, 1.0f, 0.0f) * Mat4::rotationX(-0.5f * pi), ClothSettings{}));
                addClothPin(world.get<Cloth>(entity), 12 * 13);
                sheets.push_back(entity);
            }
            for (int tick = 0; tick < 60; ++tick) {
                stepCloths(world, 1.0f / 120.0f, jobs);
            }
            std::vector<Vec3> all;
            for (const Entity sheet : sheets) {
                const std::vector<Vec3>& positions = world.get<Cloth>(sheet).particles.positions;
                all.insert(all.end(), positions.begin(), positions.end());
            }
            return all;
        };

        JobSystem serial{0};
        JobSystem parallel{7};
        const std::vector<Vec3> one = run(serial);
        const std::vector<Vec3> many = run(parallel);
        assert(std::memcmp(one.data(), many.data(), one.size() * sizeof(Vec3)) == 0);
    }
}

void testClothParallel() {
    testConstraintsAreColoured();
    testManyThreadsMatchOne();
    testClothsStepTogether();
}
