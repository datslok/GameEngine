#include "cloth/cloth.h"
#include "cloth/cloth_constraints.h"
#include "scene/mesh.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

namespace {
    constexpr float pi = std::numbers::pi_v<float>;

    bool near(float actual, float expected, float tolerance) {
        return std::abs(actual - expected) <= tolerance;
    }

    // A hinge along x from the origin to (1, 0, 0): one wing at +y, the other turned by angle about the edge from -y.
    std::array<Vec3, 4> hinge(float angle) {
        return {Vec3{0.5f, 1.0f, 0.0f}, Vec3{0.5f, -std::cos(angle), std::sin(angle)}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}};
    }

    /*
    * Every inside edge of a sheet is a hinge (the edges round its border have one triangle), each between its two
    * triangles' far corners, flat at rest, its stiffness share 3 l^2 / A from its rest shape.
    */
    void testHingesOfASheet() {
        const Mesh sheet = Mesh::grid(3, 2, 3.0f, 2.0f);
        const std::vector<BendConstraint> hinges = bendConstraints(sheet);
        const std::size_t edges = 3 * 3 + 4 * 2 + 3 * 2;
        assert(hinges.size() == edges - 2 * (3 + 2));

        for (const BendConstraint& bend : hinges) {
            assert(near(bend.restAngle, 0.0f, 1e-6f));
            const auto at = [&](std::uint32_t index) {
                return Vec3{sheet.vertices[index].x, sheet.vertices[index].y, sheet.vertices[index].z};
            };
            const float length = (at(bend.edgeEnd) - at(bend.edgeStart)).length();
            const float area = 0.5f * ((at(bend.edgeStart) - at(bend.wing0)).cross(at(bend.edgeEnd) - at(bend.wing0)).length() +
                                       (at(bend.edgeStart) - at(bend.wing1)).cross(at(bend.edgeEnd) - at(bend.wing1)).length());
            assert(near(bend.stiffnessShare, 3.0f * length * length / area, 1e-4f));
        }
    }

    // The angle between the two triangles: 0 flat, a quarter turn folded either way with opposite signs, near a half turn
    // folded almost shut.
    void testHingeAngles() {
        const auto angleOf = [](float folded) {
            const std::array<Vec3, 4> points = hinge(folded);
            return hingeAngle(points[0], points[1], points[2], points[3]);
        };
        assert(near(angleOf(0.0f), 0.0f, 1e-6f));
        assert(near(std::abs(angleOf(0.5f * pi)), 0.5f * pi, 1e-5f));
        assert(near(angleOf(-0.5f * pi), -angleOf(0.5f * pi), 1e-5f));
        assert(near(std::abs(angleOf(0.9f * pi)), 0.9f * pi, 1e-4f));
    }

    /*
    * The closed-form gradient (Bridson et al. 2003) against finite differences: each point nudged a little each way along
    * each axis, the change in angle over the nudge. They also sum to zero, so bending cannot push the cloth along.
    */
    void testGradientsMatchFiniteDifferences() {
        std::array<Vec3, 4> points{Vec3{0.4f, 0.9f, 0.2f}, Vec3{0.7f, -0.8f, 0.5f}, Vec3{0.05f, 0.1f, -0.1f}, Vec3{1.1f, -0.05f, 0.1f}};
        const std::array<Vec3, 4> gradient = hingeAngleGradient(points[0], points[1], points[2], points[3]);

        Vec3 sum{0.0f, 0.0f, 0.0f};
        for (int point = 0; point < 4; ++point) {
            sum = sum + gradient[point];
            for (int axis = 0; axis < 3; ++axis) {
                const float nudge = 1e-3f;
                std::array<Vec3, 4> plus = points;
                std::array<Vec3, 4> minus = points;
                float* plusAxis = &plus[point].x;
                float* minusAxis = &minus[point].x;
                plusAxis[axis] += nudge;
                minusAxis[axis] -= nudge;
                const float change = (hingeAngle(plus[0], plus[1], plus[2], plus[3]) - hingeAngle(minus[0], minus[1], minus[2], minus[3])) / (2.0f * nudge);
                const float analytic = (&gradient[point].x)[axis];
                assert(near(analytic, change, 2e-3f + 0.01f * std::abs(change)));
            }
        }
        assert(sum.length() < 1e-4f);
    }

    // A folded hinge with no give (and free points) opens back towards flat, all the way over repeated solves.
    void testFoldedHingeOpens() {
        const std::array<Vec3, 4> folded = hinge(0.6f * pi);
        ClothParticles particles;
        particles.positions.assign(folded.begin(), folded.end());
        particles.previousPositions = particles.positions;
        particles.velocities.assign(4, Vec3{0.0f, 0.0f, 0.0f});
        particles.inverseMasses.assign(4, 1.0f);
        const std::vector<BendConstraint> hinges{BendConstraint{0, 1, 2, 3, 0.0f, 3.0f}};

        const float before = std::abs(hingeAngle(particles.positions[0], particles.positions[1], particles.positions[2], particles.positions[3]));
        solveBend(particles, hinges, 1e6f, 0.001f);
        const float once = std::abs(hingeAngle(particles.positions[0], particles.positions[1], particles.positions[2], particles.positions[3]));
        assert(once < 0.5f * before);
        for (int solve = 0; solve < 20; ++solve) {
            solveBend(particles, hinges, 1e6f, 0.001f);
        }
        assert(std::abs(hingeAngle(particles.positions[0], particles.positions[1], particles.positions[2], particles.positions[3])) < 1e-3f);
    }

    // How far a strip clamped along its first two columns, sticking out level, droops at its free end once it settles.
    float cantileverDroop(int columns, int rows, float bendingStiffness, int substeps = 15) {
        ClothSettings settings;
        settings.bendingStiffness = bendingStiffness;
        settings.drag = 3.0f;
        settings.substeps = substeps;
        const Mat4 level = Mat4::rotationX(-0.5f * pi);
        Cloth cloth = makeCloth(Mesh::grid(columns, rows, 0.3f, 0.1f), level, settings);
        for (int row = 0; row <= rows; ++row) {
            for (int column = 0; column <= 1; ++column) {
                pinParticle(cloth.particles, static_cast<std::size_t>(row * (columns + 1) + column));
            }
        }
        for (int tick = 0; tick < 480; ++tick) {
            stepCloth(cloth, 1.0f / 120.0f);
        }
        return -cloth.particles.positions[static_cast<std::size_t>(columns)].y;
    }

    /*
    * Stiffer fabric droops less. And the same stiffness should droop alike at two mesh resolutions (the hinges' stiffness
    * share makes the bending energy independent of the mesh), but one Gauss-Seidel pass a substep carries stiffness
    * only about a hinge a pass, so a finer mesh needs more passes: for this very stiff strip (B 5 mN m, a card) the two
    * meshes droop 75% apart at 15 substeps and 20% at 60 (measured). The gap must shrink as the substeps grow.
    */
    void testCantilever() {
        const float stiff = cantileverDroop(12, 4, 5e-3f);
        const float soft = cantileverDroop(12, 4, 5e-4f);
        const float limp = cantileverDroop(12, 4, 0.0f);
        assert(stiff > 0.0f);
        assert(stiff < soft && soft < limp);

        const float fewGap = std::abs(cantileverDroop(24, 8, 5e-3f) - stiff) / stiff;
        const float coarse = cantileverDroop(12, 4, 5e-3f, 60);
        const float manyGap = std::abs(cantileverDroop(24, 8, 5e-3f, 60) - coarse) / coarse;
        assert(manyGap < 0.5f * fewGap);
        assert(manyGap < 0.25f);
    }

    /*
    * A moving pin slides evenly through the tick's substeps to where the game put it, so its velocity is its motion over
    * the tick, and its neighbours are carried along: a strip hung from two pins moving sideways together follows them.
    */
    void testMovingPins() {
        ClothSettings settings;
        settings.gravity = 0.0f;
        settings.drag = 3.0f;
        Cloth cloth = makeCloth(Mesh::grid(4, 4, 1.0f, 1.0f), Mat4::identity(), settings);
        addClothPin(cloth, 20);
        addClothPin(cloth, 24);
        assert(cloth.particles.inverseMasses[20] == 0.0f && cloth.particles.inverseMasses[24] == 0.0f);
        assert(cloth.pins.size() == 2);

        const Vec3 start = cloth.particles.positions[20];
        const Vec3 otherStart = cloth.particles.positions[24];
        for (int tick = 1; tick <= 240; ++tick) {
            const Vec3 along{0.005f * tick, 0.0f, 0.0f};
            cloth.pins[0].position = start + along;
            cloth.pins[1].position = otherStart + along;
            stepCloth(cloth, 1.0f / 120.0f);
            assert(near(cloth.particles.positions[20].x, start.x + 0.005f * tick, 1e-5f));
            assert(near(cloth.particles.velocities[20].x, 0.6f, 1e-3f));
        }
        // The rest of the strip came along at the pins' speed.
        assert(near(cloth.particles.velocities[2].x, 0.6f, 0.05f));

        bool threw = false;
        try {
            addClothPin(cloth, 25);
        } catch (const std::out_of_range&) {
            threw = true;
        }
        assert(threw);
    }

    // With bending and moving pins, the same run twice gives the same bits.
    void testDeterministic() {
        const auto run = [] {
            Cloth cloth = makeCloth(Mesh::grid(8, 8, 1.0f, 1.0f), Mat4::rotationX(-0.5f * pi), ClothSettings{});
            addClothPin(cloth, 72);
            addClothPin(cloth, 80);
            for (int tick = 0; tick < 120; ++tick) {
                cloth.pins[1].position = cloth.pins[1].position + Vec3{-0.002f, 0.0f, 0.0f};
                stepCloth(cloth, 1.0f / 120.0f);
            }
            return cloth.particles.positions;
        };
        const std::vector<Vec3> first = run();
        const std::vector<Vec3> second = run();
        assert(std::memcmp(first.data(), second.data(), first.size() * sizeof(Vec3)) == 0);
    }
}

void testClothBending() {
    testHingesOfASheet();
    testHingeAngles();
    testGradientsMatchFiniteDifferences();
    testFoldedHingeOpens();
    testCantilever();
    testMovingPins();
    testDeterministic();
}
