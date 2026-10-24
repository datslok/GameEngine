#include "cloth/cloth.h"
#include "cloth/cloth_constraints.h"
#include "cloth/cloth_particles.h"
#include "scene/mesh.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <numbers>
#include <set>
#include <utility>
#include <vector>

namespace {
    bool near(float actual, float expected, float tolerance) {
        return std::abs(actual - expected) <= tolerance;
    }

    // Points by hand: where they are, at rest, with these inverse masses.
    ClothParticles pointsAt(const std::vector<Vec3>& positions, const std::vector<float>& inverseMasses) {
        ClothParticles particles;
        particles.positions = positions;
        particles.previousPositions = positions;
        particles.velocities.assign(positions.size(), Vec3{0.0f, 0.0f, 0.0f});
        particles.inverseMasses = inverseMasses;
        return particles;
    }

    // Every edge of a grid once: its rows' and columns' threads and one diagonal a square, each at its rest length.
    void testEdgesOfAGrid() {
        const Mesh sheet = Mesh::grid(3, 2, 3.0f, 2.0f);
        const std::vector<StretchConstraint> edges = stretchConstraints(sheet);
        assert(edges.size() == 3 * 3 + 4 * 2 + 3 * 2);

        std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
        for (const StretchConstraint& edge : edges) {
            assert(edge.first < edge.second);
            assert(seen.insert({edge.first, edge.second}).second);
            const Vec4& a = sheet.vertices[edge.first];
            const Vec4& b = sheet.vertices[edge.second];
            assert(near(edge.restLength, (Vec3{a.x, a.y, a.z} - Vec3{b.x, b.y, b.z}).length(), 1e-6f));
        }
    }

    /*
    * A perfectly stiff edge (compliance 0) is put back to its length in one solve, its ends moving along it in
    * proportion to their inverse masses, so the centre of mass stays where it was.
    */
    void testStiffEdgeKeepsItsLength() {
        ClothParticles particles = pointsAt({Vec3{0.0f, 0.0f, 0.0f}, Vec3{3.0f, 0.0f, 0.0f}}, {1.0f, 0.5f});
        const std::vector<StretchConstraint> edges{{0, 1, 2.0f}};
        solveStretch(particles, edges, 0.0f, 0.01f);

        assert(near((particles.positions[1] - particles.positions[0]).length(), 2.0f, 1e-5f));
        assert(near(particles.positions[0].x, 2.0f / 3.0f, 1e-5f));
        assert(near(particles.positions[1].x, 3.0f - 1.0f / 3.0f, 1e-5f));
        const float centre = (particles.positions[0].x * 1.0f + particles.positions[1].x * 2.0f) / 3.0f;
        assert(near(centre, (0.0f + 6.0f) / 3.0f, 1e-5f));
    }

    // A pinned end never moves; an edge with both ends pinned is left alone (its correction would divide by zero).
    void testPinnedEndsStay() {
        ClothParticles particles = pointsAt({Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, -3.0f, 0.0f}}, {0.0f, 1.0f});
        solveStretch(particles, {{0, 1, 2.0f}}, 0.0f, 0.01f);
        assert(particles.positions[0].x == 0.0f && particles.positions[0].y == 0.0f);
        assert(near(particles.positions[1].y, -2.0f, 1e-5f));

        ClothParticles fixed = pointsAt({Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, -3.0f, 0.0f}}, {0.0f, 0.0f});
        solveStretch(fixed, {{0, 1, 2.0f}}, 0.0f, 0.01f);
        assert(fixed.positions[1].y == -3.0f);
    }

    /*
    * Hooke's law, the reason for XPBD: a weight of m kg hanging from a pinned point by an edge of compliance alpha
    * (m/N) settles stretched by m g alpha, the same whether a tick takes 5, 15 or 30 substeps. Within 1%: each substep
    * gravity moves the weight by g h^2, a few micrometres, which a float a metre from the origin resolves only to about
    * 0.1 micrometre, and the stretch settles where the rounded moves balance (measured 0.4% to 0.5% off).
    */
    void testHangingWeightStretchesByHookesLaw() {
        const float mass = 0.5f;
        const float compliance = 0.002f;
        ClothSettings settings;
        settings.stretchCompliance = compliance;
        settings.drag = 5.0f;

        for (int substeps : {5, 15, 30}) {
            settings.substeps = substeps;
            ClothParticles particles = pointsAt({Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}}, {0.0f, 1.0f / mass});
            const std::vector<StretchConstraint> edges{{0, 1, 1.0f}};
            for (int tick = 0; tick < 1200; ++tick) {
                stepClothParticles(particles, edges, settings, 1.0f / 120.0f);
            }
            const float stretch = -particles.positions[1].y - 1.0f;
            const float expected = mass * settings.gravity * compliance;
            assert(near(stretch, expected, 0.01f * expected));
        }
    }

    // A stiff pendulum keeps its length while it swings, and drag slows it.
    void testPendulumKeepsItsLength() {
        ClothSettings settings;
        settings.stretchCompliance = 0.0f;
        settings.drag = 0.0f;
        ClothParticles particles = pointsAt({Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}, {0.0f, 1.0f});
        const std::vector<StretchConstraint> edges{{0, 1, 1.0f}};

        float fastest = 0.0f;
        for (int tick = 0; tick < 240; ++tick) {
            stepClothParticles(particles, edges, settings, 1.0f / 120.0f);
            assert(near(particles.positions[1].length(), 1.0f, 1e-4f));
            fastest = std::max(fastest, particles.velocities[1].length());
        }

        // Without drag it still swings near its full speed at the bottom (sqrt(2 g L)); with drag it slows.
        assert(fastest > 0.9f * std::sqrt(2.0f * settings.gravity));
        ClothSettings dragged = settings;
        dragged.drag = 2.0f;
        ClothParticles slowed = pointsAt({Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}, {0.0f, 1.0f});
        float fastestDragged = 0.0f;
        for (int tick = 0; tick < 240; ++tick) {
            stepClothParticles(slowed, edges, dragged, 1.0f / 120.0f);
            fastestDragged = std::max(fastestDragged, slowed.velocities[1].length());
        }
        assert(fastestDragged < 0.9f * fastest);
    }

    /*
    * A sheet laid flat (its top edge at the back), pinned at its two back corners, falls, swings down and hangs between
    * them, barely stretched (cotton's compliance); the same run twice gives the same bits.
    */
    void testPinnedSheetHangs() {
        const Mesh sheet = Mesh::grid(10, 10, 1.0f, 1.0f);
        const Mat4 flat = Mat4::translation(0.0f, 2.0f, 0.0f) * Mat4::rotationX(-0.5f * std::numbers::pi_v<float>);
        const auto hang = [&] {
            Cloth cloth = makeCloth(sheet, flat, ClothSettings{});
            pinParticle(cloth.particles, 10 * 11);
            pinParticle(cloth.particles, 10 * 11 + 10);
            for (int tick = 0; tick < 600; ++tick) {
                stepCloth(cloth, 1.0f / 120.0f);
            }
            return cloth;
        };

        const Cloth cloth = hang();
        assert(near(cloth.particles.positions[110].y, 2.0f, 1e-6f) && near(cloth.particles.positions[110].z, -0.5f, 1e-6f));
        assert(near(cloth.particles.positions[120].x, 0.5f, 1e-6f));

        float worstStretch = 0.0f;
        for (const StretchConstraint& edge : cloth.stretch) {
            const float length = (cloth.particles.positions[edge.first] - cloth.particles.positions[edge.second]).length();
            worstStretch = std::max(worstStretch, length / edge.restLength - 1.0f);
        }
        assert(worstStretch < 0.01f);

        // Nothing rises above the pins, and the far edge has swung down to hang below them.
        for (std::size_t index = 0; index < cloth.particles.size(); ++index) {
            assert(cloth.particles.positions[index].y <= 2.0f + 1e-4f);
        }
        assert(cloth.particles.positions[5].y < 1.3f);

        const Cloth again = hang();
        assert(std::memcmp(again.particles.positions.data(), cloth.particles.positions.data(), cloth.particles.size() * sizeof(Vec3)) == 0);
    }
}

void testClothSolver() {
    testEdgesOfAGrid();
    testStiffEdgeKeepsItsLength();
    testPinnedEndsStay();
    testHangingWeightStretchesByHookesLaw();
    testPendulumKeepsItsLength();
    testPinnedSheetHangs();
}
