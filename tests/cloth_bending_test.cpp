#include "cloth/cloth.h"
#include "cloth/cloth_constraints.h"
#include "scene/mesh.h"

#include "cloth_helpers.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>
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

    // A hinge's points with the given inverse masses.
    ClothParticles hingeParticles(const std::array<Vec3, 4>& points, const std::array<float, 4>& inverseMasses) {
        ClothParticles particles;
        particles.positions.assign(points.begin(), points.end());
        particles.previousPositions = particles.positions;
        particles.velocities.assign(4, Vec3{0.0f, 0.0f, 0.0f});
        particles.inverseMasses.assign(inverseMasses.begin(), inverseMasses.end());
        return particles;
    }

    float angleOf(const ClothParticles& particles) {
        return std::abs(hingeAngle(particles.positions[0], particles.positions[1], particles.positions[2], particles.positions[3]));
    }

    // A folded hinge with no give (and free points) opens back towards flat, all the way over repeated solves.
    void foldedHingeOpens(ClothParticles& particles, const std::vector<BendConstraint>& hinges, BendingModel model) {
        const float before = angleOf(particles);
        solveBend(particles, hinges, 1e6f, 0.001f, model);
        assert(angleOf(particles) < 0.5f * before);
        for (int solve = 0; solve < 20; ++solve) {
            solveBend(particles, hinges, 1e6f, 0.001f, model);
        }
        assert(angleOf(particles) < 1e-3f);
    }

    void testFoldedHingeOpens() {
        ClothParticles particles = hingeParticles(hinge(0.6f * pi), {1.0f, 1.0f, 1.0f, 1.0f});
        foldedHingeOpens(particles, {BendConstraint{0, 1, 2, 3, 0.0f, 3.0f}}, BendingModel::Hinge);
    }

    // How far a strip clamped along its first two columns, sticking out level, droops at its free end once it settles.
    float cantileverDroop(int columns, int rows, float bendingStiffness, int substeps = 15, BendingModel model = BendingModel::Hinge,
                          int bendingInterval = 1) {
        ClothSettings settings = withoutSelfCollision();
        settings.bendingStiffness = bendingStiffness;
        settings.bendingModel = model;
        settings.drag = 3.0f;
        settings.substeps = substeps;
        settings.bendingInterval = bendingInterval;
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

    /*
    * Isometric bending's weights (Bergou et al. 2006, scaled by -1 / l): each wing 1 / its distance from the edge, each
    * end of the edge minus the wings' weights shared by where along the edge they lie. They sum to zero and send the
    * flat rest hinge to C = 0, so a flat sheet feels no bending.
    */
    void testIsometricWeights() {
        const std::array<Vec3, 4> square = hinge(0.0f);
        const std::array<float, 4> even = isometricBendWeights(square[0], square[1], square[2], square[3]);
        assert(near(even[0], 1.0f, 1e-6f) && near(even[1], 1.0f, 1e-6f));
        assert(near(even[2], -1.0f, 1e-6f) && near(even[3], -1.0f, 1e-6f));

        // Wings at different heights and places along the edge: 1 / 0.5 and 1 / 2, at 0.2 and 0.9 of the way.
        const std::array<float, 4> skewed =
            isometricBendWeights(Vec3{0.2f, 0.5f, 0.0f}, Vec3{0.9f, -2.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f});
        assert(near(skewed[0], 2.0f, 1e-5f) && near(skewed[1], 0.5f, 1e-5f));
        assert(near(skewed[2], -1.65f, 1e-5f) && near(skewed[3], -0.85f, 1e-5f));

        // Every hinge of a sheet at rest, and the sheet moved and turned as a whole: no curvature.
        const Mesh sheet = Mesh::grid(3, 2, 3.0f, 2.0f);
        const Mat4 moved = Mat4::translation(1.0f, 2.0f, -3.0f) * Mat4::rotationX(0.7f) * Mat4::rotationY(-1.1f);
        for (const BendConstraint& bend : bendConstraints(sheet)) {
            const std::array<float, 4>& weights = bend.isometricWeights;
            assert(near(weights[0] + weights[1] + weights[2] + weights[3], 0.0f, 1e-5f));
            const auto at = [&](std::uint32_t index) {
                return transformPoint(moved, Vec3{sheet.vertices[index].x, sheet.vertices[index].y, sheet.vertices[index].z});
            };
            assert(isometricCurvature(weights, at(bend.wing0), at(bend.wing1), at(bend.edgeStart), at(bend.edgeEnd)).length() < 1e-5f);
        }

        // The weights come from each triangle's own shape, so a hinge made folded still rests flat.
        const std::array<Vec3, 4> folded = hinge(0.6f * pi);
        const std::array<float, 4> fromFolded = isometricBendWeights(folded[0], folded[1], folded[2], folded[3]);
        for (int point = 0; point < 4; ++point) {
            assert(near(fromFolded[point], even[point], 1e-5f));
        }
    }

    /*
    * A hinge folded by an angle without stretching has |C| = 2 sin(angle / 2): the angle itself while small, so its
    * energy matches the angle model's for the same stiffness, and a little softer in deep folds (1.41 against 1.57 at a
    * quarter turn).
    */
    void testIsometricCurvature() {
        const std::array<Vec3, 4> flat = hinge(0.0f);
        const std::array<float, 4> weights = isometricBendWeights(flat[0], flat[1], flat[2], flat[3]);
        for (const float angle : {0.01f, 0.5f * pi, -0.5f * pi, 0.9f * pi}) {
            const std::array<Vec3, 4> points = hinge(angle);
            const float size = isometricCurvature(weights, points[0], points[1], points[2], points[3]).length();
            assert(near(size, 2.0f * std::abs(std::sin(0.5f * angle)), 1e-5f));
        }
        const std::array<Vec3, 4> slight = hinge(0.01f);
        assert(near(isometricCurvature(weights, slight[0], slight[1], slight[2], slight[3]).length(),
                    std::abs(hingeAngle(slight[0], slight[1], slight[2], slight[3])), 1e-6f));
    }

    Vec3 centreOfMass(const ClothParticles& particles) {
        Vec3 sum{0.0f, 0.0f, 0.0f};
        float mass = 0.0f;
        for (std::size_t point = 0; point < particles.size(); ++point) {
            sum = sum + particles.positions[point] * (1.0f / particles.inverseMasses[point]);
            mass += 1.0f / particles.inverseMasses[point];
        }
        return sum * (1.0f / mass);
    }

    /*
    * A folded hinge opens back to flat under isometric bending too, and since the weights sum to zero the centre of mass
    * stays put whatever the masses. Pinned points never move.
    */
    void testIsometricFoldOpens() {
        const std::array<Vec3, 4> folded = hinge(0.6f * pi);
        const std::array<Vec3, 4> flat = hinge(0.0f);
        BendConstraint bend{0, 1, 2, 3, 0.0f, 3.0f};
        bend.isometricWeights = isometricBendWeights(flat[0], flat[1], flat[2], flat[3]);
        const std::vector<BendConstraint> hinges{bend};

        ClothParticles particles = hingeParticles(folded, {1.0f, 2.0f, 0.5f, 1.0f});
        const Vec3 centre = centreOfMass(particles);
        foldedHingeOpens(particles, hinges, BendingModel::Isometric);
        assert((centreOfMass(particles) - centre).length() < 1e-5f);

        // The edge pinned: only the wings move. Everything pinned: nothing does.
        ClothParticles edgePinned = hingeParticles(folded, {1.0f, 1.0f, 0.0f, 0.0f});
        solveBend(edgePinned, hinges, 1e6f, 0.001f, BendingModel::Isometric);
        assert(std::memcmp(&edgePinned.positions[2], &folded[2], 2 * sizeof(Vec3)) == 0);
        assert((edgePinned.positions[1] - folded[1]).length() > 0.0f);
        ClothParticles allPinned = hingeParticles(folded, {0.0f, 0.0f, 0.0f, 0.0f});
        solveBend(allPinned, hinges, 1e6f, 0.001f, BendingModel::Isometric);
        assert(std::memcmp(allPinned.positions.data(), folded.data(), sizeof(folded)) == 0);
    }

    /*
    * Isometric bending is the default, and a strip under it droops like one under the angle model: stiffer less, and for
    * the same stiffness near the angle model's droop.
    */
    void testIsometricCantilever() {
        assert(ClothSettings{}.bendingModel == BendingModel::Isometric);

        const float stiff = cantileverDroop(12, 4, 5e-3f, 15, BendingModel::Isometric);
        const float soft = cantileverDroop(12, 4, 5e-4f, 15, BendingModel::Isometric);
        assert(stiff > 0.0f && stiff < soft);

        for (const float stiffness : {5e-3f, 5e-4f}) {
            const float isometric = cantileverDroop(12, 4, stiffness, 15, BendingModel::Isometric);
            const float angle = cantileverDroop(12, 4, stiffness, 15, BendingModel::Hinge);
            assert(std::abs(isometric - angle) < 0.15f * angle);
        }
    }

    /*
    * Which substeps solve the bending, and for how many substeps each stands: every interval-th, the last taking what is
    * left, so together they cover the tick once. An interval of 1 bends every substep; one past the substeps bends once.
    */
    void testBendingSubsteps() {
        for (int step = 0; step < 15; ++step) {
            assert(bendingSubsteps(step, 15, 1) == 1);
        }

        const std::array<int, 15> everyFourth{0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 3};
        for (int step = 0; step < 15; ++step) {
            assert(bendingSubsteps(step, 15, 4) == everyFourth[static_cast<std::size_t>(step)]);
        }

        for (const int substeps : {1, 4, 15, 16, 60}) {
            for (const int interval : {1, 2, 3, 4, 7, 100}) {
                int covered = 0;
                for (int step = 0; step < substeps; ++step) {
                    covered += bendingSubsteps(step, substeps, interval);
                }
                assert(covered == substeps);
            }
        }
        assert(bendingSubsteps(14, 15, 20) == 15 && bendingSubsteps(13, 15, 20) == 0);

        for (const int interval : {0, -3}) {
            bool threw = false;
            try {
                bendingSubsteps(0, 15, interval);
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            assert(threw);
        }
    }

    /*
    * Bending solved every fourth substep is the same fabric: each solve stands for four substeps, so its compliance over
    * the substep squared is divided by four, giving the impulse four substeps' bending would (XPBD turns a correction
    * into velocity over one substep). A strip droops about as far either way, while bending every fourth substep without
    * that, a fabric four times softer, droops measurably further.
    */
    void testBendingInterval() {
        assert(ClothSettings{}.bendingInterval >= 1);
        for (const BendingModel model : {BendingModel::Isometric, BendingModel::Hinge}) {
            const float everySubstep = cantileverDroop(12, 4, 5e-4f, 60, model, 1);
            const float everyFourth = cantileverDroop(12, 4, 5e-4f, 60, model, 4);
            const float softer = cantileverDroop(12, 4, 5e-4f / 4.0f, 60, model, 1);
            assert(std::abs(everyFourth - everySubstep) < 0.05f * everySubstep);
            assert(softer - everySubstep > 4.0f * std::abs(everyFourth - everySubstep));
        }
    }

    // With bending and moving pins, the same run twice gives the same bits.
    void testDeterministic() {
        const auto run = [] {
            Cloth cloth = makeCloth(Mesh::grid(8, 8, 1.0f, 1.0f), Mat4::rotationX(-0.5f * pi), withoutSelfCollision());
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
    testBendingSubsteps();
    testBendingInterval();
    testMovingPins();
    testIsometricWeights();
    testIsometricCurvature();
    testIsometricFoldOpens();
    testIsometricCantilever();
    testDeterministic();
}
