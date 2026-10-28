#include "cloth/cloth.h"
#include "cloth/cloth_seams.h"
#include "math/angles.h"
#include "physics/shapes.h"
#include "scene/mesh.h"
#include "test_levels.h"

#include <algorithm>
#include <cmath>
#include <cassert>
#include <cstdint>
#include <vector>

namespace {
    constexpr float tick = 1.0f / 120.0f;

    // Two free points with the given inverse masses at the given places, still.
    ClothParticles twoPoints(const Vec3& first, const Vec3& second, float firstInverseMass, float secondInverseMass) {
        ClothParticles particles;
        particles.positions = {first, second};
        particles.previousPositions = particles.positions;
        particles.velocities = {Vec3{}, Vec3{}};
        particles.inverseMasses = {firstInverseMass, secondInverseMass};
        return particles;
    }

    bool sameBits(const Vec3& a, const Vec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    // A stitch further apart than a step closes by just the step, along the line between its points, each moving by its share of the inverse mass.
    void testStitchClosesByAStep() {
        ClothParticles even = twoPoints(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 1.0f, 1.0f);
        solveStitches(even, {ClothStitch{0, 1}}, 0.1f);
        assert(near(even.positions[0].x, 0.05f, 1e-6f) && near(even.positions[1].x, 0.95f, 1e-6f));
        assert(even.positions[0].y == 0.0f && even.positions[1].z == 0.0f);

        // The lighter point (inverse mass 3) moves three times as far.
        ClothParticles uneven = twoPoints(Vec3{0.0f, 2.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}, 1.0f, 3.0f);
        solveStitches(uneven, {ClothStitch{0, 1}}, 0.4f);
        assert(near(uneven.positions[0].y, 1.9f, 1e-6f) && near(uneven.positions[1].y, 0.3f, 1e-6f));
    }

    /*
    * The step is measured from where the points were when the substep began: points that have already closed in by
    * more than a step (their own speed) are left alone, and points that drifted apart are pulled back to a step
    * closer than they began.
    */
    void testStitchMeasuredFromTheSubstepStart() {
        ClothParticles closing = twoPoints(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 1.0f, 1.0f);
        closing.positions = {Vec3{0.2f, 0.0f, 0.0f}, Vec3{0.8f, 0.0f, 0.0f}};
        solveStitches(closing, {ClothStitch{0, 1}}, 0.1f);
        assert(closing.positions[0].x == 0.2f && closing.positions[1].x == 0.8f);

        ClothParticles drifting = twoPoints(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 1.0f, 1.0f);
        drifting.positions = {Vec3{-0.5f, 0.0f, 0.0f}, Vec3{1.5f, 0.0f, 0.0f}};
        solveStitches(drifting, {ClothStitch{0, 1}}, 0.1f);
        assert(near(drifting.positions[0].x, 0.05f, 1e-6f) && near(drifting.positions[1].x, 0.95f, 1e-6f));
    }

    // Within a step the stitch shuts: both points land on the same spot, to the last bit, where the masses balance.
    void testStitchWithinAStepShuts() {
        ClothParticles particles = twoPoints(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.04f}, 1.0f, 3.0f);
        solveStitches(particles, {ClothStitch{0, 1}}, 0.1f);
        assert(sameBits(particles.positions[0], particles.positions[1]));
        assert(near(particles.positions[0].z, 0.01f, 1e-6f));

        // Shut, it stays shut.
        solveStitches(particles, {ClothStitch{0, 1}}, 0.1f);
        assert(sameBits(particles.positions[0], particles.positions[1]) && near(particles.positions[0].z, 0.01f, 1e-6f));
    }

    // A pinned point never moves: the free one comes all the way; two pinned points stay apart.
    void testPinnedStitch() {
        ClothParticles oneHeld = twoPoints(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 0.0f, 1.0f);
        solveStitches(oneHeld, {ClothStitch{0, 1}}, 0.25f);
        assert(oneHeld.positions[0].x == 0.0f && near(oneHeld.positions[1].x, 0.75f, 1e-6f));

        ClothParticles bothHeld = twoPoints(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 0.0f, 0.0f);
        solveStitches(bothHeld, {ClothStitch{0, 1}}, 0.25f);
        assert(bothHeld.positions[0].x == 0.0f && bothHeld.positions[1].x == 1.0f);
    }

    /*
    * A stitch is being sewn while its gap at the substep's start is wider than the step: both its points are marked, so
    * the bodies push them out without friction. Shut stitches and points in none are not marked.
    */
    void testSewingPointsAreMarked() {
        ClothParticles particles;
        particles.positions = {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.3f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.05f, 0.0f, 0.0f}, Vec3{2.0f, 0.0f, 0.0f}};
        particles.previousPositions = particles.positions;
        particles.velocities.assign(5, Vec3{});
        particles.inverseMasses.assign(5, 1.0f);

        std::vector<std::uint8_t> sewing;
        markSewing(particles, {ClothStitch{0, 1}, ClothStitch{2, 3}}, 0.1f, sewing);
        assert((sewing == std::vector<std::uint8_t>{1, 1, 0, 0, 0}));

        // Measured from where the points began the substep, as the stitch is.
        particles.positions[1] = Vec3{0.05f, 0.0f, 0.0f};
        particles.previousPositions[3] = Vec3{1.5f, 0.0f, 0.0f};
        markSewing(particles, {ClothStitch{0, 1}, ClothStitch{2, 3}}, 0.1f, sewing);
        assert((sewing == std::vector<std::uint8_t>{1, 1, 1, 1, 0}));

        // The widest a stitch may end the substep: its start's gap less the step, nothing or less once shut.
        assert(near(stitchWidest(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.3f, 0.0f}, 0.1f), 0.2f, 1e-6f));
        assert(stitchWidest(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.05f, 0.0f}, 0.1f) <= 0.0f);
    }

    /*
    * Sewing round a body: two small pieces lying on a ball 140 degrees apart, stitched corner to corner. The stitches
    * pull along the chord, nearly straight into the ball (a slide of 0.36 of the push, under the static friction's 0.6),
    * so with friction the points would stick where they lie and the seam stay open. A seam being sewn slides freely, as
    * a sewing machine pulls fabric over a table: the pieces slide up the ball and meet at its top.
    */
    void testSeamSewsRoundABall() {
        const float radius = 0.1f;
        ClothSettings settings;
        settings.gravity = 0.0f;
        const std::vector<Shape> ball{Shape{Sphere{Vec3{0.0f, 0.0f, 0.0f}, radius}}};

        // Each piece a 1 cm triangle at rest; on the ball at 70 degrees either side of its top, one mirroring the other.
        Mesh rest;
        rest.vertices = {Vec4{0.0f, 0.0f, 0.0f, 1.0f}, Vec4{0.01f, 0.0f, 0.0f, 1.0f}, Vec4{0.0f, 0.0f, 0.01f, 1.0f},
                         Vec4{1.0f, 0.0f, 0.0f, 1.0f}, Vec4{1.01f, 0.0f, 0.0f, 1.0f}, Vec4{1.0f, 0.0f, 0.01f, 1.0f}};
        rest.triangles = {Triangle{0, 1, 2}, Triangle{3, 5, 4}};
        const float lying = radius + settings.thickness;
        const auto onBall = [&](float degrees, float z) {
            const float angle = radiansFromDegrees(degrees);
            return Vec3{lying * std::sin(angle), lying * std::cos(angle), z};
        };
        const std::vector<Vec3> start{onBall(70.0f, 0.0f), onBall(75.5f, 0.0f), onBall(70.0f, 0.01f),
                                      onBall(-70.0f, 0.0f), onBall(-75.5f, 0.0f), onBall(-70.0f, 0.01f)};
        ClothSewing sewing;
        sewing.stitches = {ClothStitch{0, 3}, ClothStitch{1, 4}, ClothStitch{2, 5}};
        Cloth cloth = makeCloth(rest, start, settings, sewing);

        for (int ticks = 0; ticks < 120; ++ticks) {
            stepCloth(cloth, tick, ball);
        }
        for (const ClothStitch& stitch : cloth.stitches) {
            assert(sameBits(cloth.particles.positions[stitch.first], cloth.particles.positions[stitch.second]));
            assert(cloth.particles.positions[stitch.first].y > 0.9f * radius);
        }
    }

    /*
    * A square cut along its diagonal into two pieces, the second turned a quarter and moved away, sewn back along the
    * diagonal: the hinge across the seam is the uncut square's hinge across its diagonal, the second piece's corner
    * unfolded flat beside the first.
    */
    void testSeamHingeIsTheUncutHinge() {
        Mesh square;
        square.vertices = {Vec4{0.0f, 0.0f, 0.0f, 1.0f}, Vec4{1.0f, 0.0f, 0.0f, 1.0f}, Vec4{1.0f, 1.0f, 0.0f, 1.0f}, Vec4{0.0f, 1.0f, 0.0f, 1.0f}};
        square.triangles = {Triangle{0, 1, 2}, Triangle{0, 2, 3}};
        const std::vector<BendConstraint> uncut = bendConstraints(square);
        assert(uncut.size() == 1 && uncut[0].wing0 == 1 && uncut[0].wing1 == 3 && uncut[0].edgeStart == 0 && uncut[0].edgeEnd == 2);

        // The first piece is the first triangle as it was; the second is points 3, 4, 5, copies of 0, 2, 3 turned a quarter about z and moved.
        const std::vector<Vec3> rest{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 0.0f},
                                     Vec3{5.0f, 0.0f, 0.0f}, Vec3{4.0f, 1.0f, 0.0f}, Vec3{4.0f, 0.0f, 0.0f}};
        const std::vector<std::array<std::uint32_t, 3>> triangles{{0, 1, 2}, {3, 4, 5}};
        const std::vector<BendConstraint> sewn = seamHinges(rest, triangles, {ClothStitch{0, 3}, ClothStitch{2, 4}});
        assert(sewn.size() == 1);
        assert(sewn[0].wing0 == 1 && sewn[0].wing1 == 5 && sewn[0].edgeStart == 0 && sewn[0].edgeEnd == 2);
        assert(near(sewn[0].stiffnessShare, uncut[0].stiffnessShare, 1e-5f));
        assert(near(sewn[0].restAngle, 0.0f, 1e-6f));
        for (int point = 0; point < 4; ++point) {
            assert(near(sewn[0].isometricWeights[point], uncut[0].isometricWeights[point], 1e-5f));
        }
    }

    /*
    * A cloth made from a rest mesh and a start: its rest shape (lengths, masses, the rest positions) is the mesh, its
    * points start where they are put, still; the seams' stitches are kept and their hinges join the bending, coloured.
    */
    void testClothFromRestAndStart() {
        const Mesh grid = Mesh::grid(2, 2, 1.0f, 1.0f);
        std::vector<Vec3> start;
        for (const Vec4& vertex : grid.vertices) {
            start.push_back(Vec3{vertex.x * 2.0f, vertex.y, 3.0f});
        }
        ClothSewing sewing;
        sewing.stitches = {ClothStitch{0, 8}};
        sewing.hinges = {BendConstraint{1, 3, 0, 4, 0.0f, 1.0f, {1.0f, 1.0f, -1.0f, -1.0f}}};

        const Cloth cloth = makeCloth(grid, start, ClothSettings{}, sewing);
        const Cloth plain = makeCloth(grid, Mat4::identity(), ClothSettings{});
        assert(cloth.particles.size() == 9);
        for (std::size_t index = 0; index < 9; ++index) {
            assert(sameBits(cloth.particles.positions[index], start[index]) && sameBits(cloth.particles.previousPositions[index], start[index]));
            assert(sameBits(cloth.restPositions[index], plain.restPositions[index]));
            assert(cloth.particles.inverseMasses[index] == plain.particles.inverseMasses[index]);
        }
        for (const StretchConstraint& edge : cloth.stretch) {
            assert(near(edge.restLength, 0.5f, 1e-6f) || near(edge.restLength, 0.70710677f, 1e-6f));
        }
        assert(cloth.stitches.size() == 1 && cloth.stitches[0].first == 0 && cloth.stitches[0].second == 8);
        assert(cloth.bend.size() == plain.bend.size() + 1);
        assert(cloth.bendColours.back() == cloth.bend.size());
    }

    /*
    * A pillow case in the air: two squares facing each other 10 cm apart, sewn along their left and right sides,
    * weightless. The stitches close at the seam speed and then hold shut: at the end of every tick after that each
    * stitch's points are one point.
    */
    void testPillowSewsShut() {
        const int squares = 6;
        const float size = 0.3f;
        const Mesh panel = Mesh::grid(squares, squares, size, size);
        const std::uint32_t perPanel = static_cast<std::uint32_t>(panel.vertices.size());

        // Both panels in one mesh, the back one's points after the front's, both facing +z at rest.
        Mesh rest = panel;
        for (const Vec4& vertex : panel.vertices) {
            rest.vertices.push_back(Vec4{vertex.x + 1.0f, vertex.y, 0.0f, 1.0f});
        }
        for (const Triangle& triangle : panel.triangles) {
            rest.triangles.push_back(Triangle{triangle.first + perPanel, triangle.second + perPanel, triangle.third + perPanel});
        }
        std::vector<Vec3> start;
        for (std::uint32_t index = 0; index < rest.vertices.size(); ++index) {
            const Vec4& vertex = panel.vertices[index % perPanel];
            start.push_back(Vec3{vertex.x, vertex.y + 1.0f, index < perPanel ? 0.05f : -0.05f});
        }

        // The left and right columns, the same rows on each panel.
        ClothSewing sewing;
        const std::uint32_t row = static_cast<std::uint32_t>(squares + 1);
        for (std::uint32_t side : {0u, static_cast<std::uint32_t>(squares)}) {
            for (std::uint32_t along = 0; along < row; ++along) {
                sewing.stitches.push_back(ClothStitch{along * row + side, perPanel + along * row + side});
            }
        }

        ClothSettings settings;
        settings.gravity = 0.0f;
        settings.seamSpeed = 1.0f;
        Cloth cloth = makeCloth(rest, start, settings, sewing);

        // 10 cm at 1 m/s takes 0.1 s, twelve ticks: half way after six, none shut; then the gap stays shut.
        const auto narrowestGap = [&] {
            float narrowest = 1.0f;
            for (const ClothStitch& stitch : cloth.stitches) {
                narrowest = std::min(narrowest, (cloth.particles.positions[stitch.first] - cloth.particles.positions[stitch.second]).length());
            }
            return narrowest;
        };
        for (int ticks = 0; ticks < 6; ++ticks) {
            stepCloth(cloth, tick);
        }
        assert(narrowestGap() > 0.04f);
        for (int ticks = 6; ticks < 16; ++ticks) {
            stepCloth(cloth, tick);
        }
        for (int ticks = 0; ticks < 30; ++ticks) {
            stepCloth(cloth, tick);
            for (const ClothStitch& stitch : cloth.stitches) {
                assert(sameBits(cloth.particles.positions[stitch.first], cloth.particles.positions[stitch.second]));
            }
        }

        // Sewn at the middle: the seams lie between the panels' starts.
        for (const ClothStitch& stitch : cloth.stitches) {
            assert(near(cloth.particles.positions[stitch.first].z, 0.0f, 0.01f));
        }
    }
}

void testClothSeams() {
    testStitchClosesByAStep();
    testStitchWithinAStepShuts();
    testStitchMeasuredFromTheSubstepStart();
    testPinnedStitch();
    testSeamHingeIsTheUncutHinge();
    testClothFromRestAndStart();
    testPillowSewsShut();
    testSewingPointsAreMarked();
    testSeamSewsRoundABall();
}
