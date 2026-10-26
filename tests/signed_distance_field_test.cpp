#include "core/job_system.h"
#include "math/mat4.h"
#include "physics/signed_distance_field.h"
#include "scene/collision_mesh.h"
#include "scene/mesh.h"
#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {
    constexpr float spacing = 0.01f;
    constexpr float band = 0.03f;

    // A sample's place in the world.
    Vec3 samplePosition(const SignedDistanceField& field, int x, int y, int z) {
        return field.origin + Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)} * field.spacing;
    }

    // A box's exact signed distance: outside, to its nearest point; inside, minus the distance to its nearest face.
    float boxDistance(const Vec3& point, const Vec3& halfSize) {
        const Vec3 q{std::abs(point.x) - halfSize.x, std::abs(point.y) - halfSize.y, std::abs(point.z) - halfSize.z};
        const Vec3 outside{std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f)};
        return outside.length() + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
    }

    /*
    * A cube is the hardest shape for telling inside from outside by the nearest feature's normal: beside its edges and
    * corners the nearest point is an edge or a corner, whose normal is the faces' around it. Every sample within the
    * band holds the exact distance with the right sign; every one beyond holds the band with the right sign.
    */
    void testCubeSamples() {
        const Vec3 halfSize{0.1f, 0.1f, 0.1f};
        const TriangleMesh cube = collisionMesh(Mesh::cube(), Mat4::scaling(halfSize.x, halfSize.y, halfSize.z));
        const SignedDistanceField field = buildSignedDistanceField(cube, spacing, band);

        // The box reaches a spacing past the band all round, so its edge is outside and far.
        assert(field.bounds().minimum.x <= -halfSize.x - band - spacing + 1e-6f);
        assert(field.bounds().maximum.y >= halfSize.y + band + spacing - 1e-6f);
        assert(field.distances.size() == static_cast<std::size_t>(field.counts[0]) * field.counts[1] * field.counts[2]);

        int near = 0;
        for (int z = 0; z < field.counts[2]; ++z) {
            for (int y = 0; y < field.counts[1]; ++y) {
                for (int x = 0; x < field.counts[0]; ++x) {
                    const float exact = boxDistance(samplePosition(field, x, y, z), halfSize);
                    const float held = field.at(x, y, z);
                    if (std::abs(exact) < band - 1e-4f) {
                        assert(std::abs(held - exact) < 1e-5f);
                        ++near;
                    } else if (std::abs(exact) > band + 1e-4f) {
                        assert(held == (exact > 0.0f ? band : -band));
                    }
                }
            }
        }
        assert(near > 1000);
    }

    /*
    * A finely divided ball: between samples the field is blended from the eight around, within the mesh's facets'
    * depth (about 1.4 mm for 64 x 32 at 0.3 m) of the true ball's distance, its slope pointing straight out. Within
    * the band less a cell's diagonal, so no corner of the cell is cut off at the band.
    */
    void testBallBetweenSamples() {
        const Vec3 centre{0.2f, 1.0f, -0.4f};
        const float radius = 0.3f;
        const TriangleMesh ball =
            collisionMesh(Mesh::sphere(64, 32), Mat4::translation(centre.x, centre.y, centre.z) * Mat4::scaling(radius, radius, radius));
        const SignedDistanceField field = buildSignedDistanceField(ball, spacing, band);

        for (int i = 0; i < 400; ++i) {
            const float theta = 0.37f * static_cast<float>(i);
            const float up = std::cos(0.61f * static_cast<float>(i));
            const float around = std::sqrt(1.0f - up * up);
            const Vec3 out{around * std::cos(theta), up, around * std::sin(theta)};
            const float depth = -0.012f + 0.024f * static_cast<float>(i % 41) / 40.0f;
            const FieldPoint point = sampleField(field, centre + out * (radius + depth));
            assert(std::abs(point.distance - depth) < 0.002f);
            assert(point.gradient.normalized().dot(out) > 0.99f);
        }

        // Far inside and far outside (out along the box's diagonal, twice the band from the ball): the band, with its sign.
        assert(sampleField(field, centre).distance == -band);
        const Vec3 diagonal = Vec3{1.0f, 1.0f, 1.0f}.normalized();
        assert(field.bounds().contains(centre + diagonal * (radius + 2.0f * band)));
        assert(sampleField(field, centre + diagonal * (radius + 2.0f * band)).distance == band);
    }

    // Outside its box a field is a band away, with no slope.
    void testOutsideTheBox() {
        const TriangleMesh cube = collisionMesh(Mesh::cube(), Mat4::scaling(0.1f, 0.1f, 0.1f));
        const SignedDistanceField field = buildSignedDistanceField(cube, spacing, band);
        for (const Vec3& far : {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, -2.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.5f}}) {
            const FieldPoint point = sampleField(field, far);
            assert(point.distance == band);
            assert(point.gradient.x == 0.0f && point.gradient.y == 0.0f && point.gradient.z == 0.0f);
        }
    }

    // On a sample the blend is that sample; a field that is linear in position is blended exactly, its slope the field's.
    void testBlending() {
        SignedDistanceField field;
        field.origin = Vec3{-1.0f, 0.5f, 2.0f};
        field.spacing = 0.25f;
        field.counts = {5, 4, 6};
        field.band = 10.0f;
        const Vec3 slope{0.3f, -0.6f, 0.2f};
        for (int z = 0; z < field.counts[2]; ++z) {
            for (int y = 0; y < field.counts[1]; ++y) {
                for (int x = 0; x < field.counts[0]; ++x) {
                    field.distances.push_back(slope.dot(samplePosition(field, x, y, z)) + 0.1f);
                }
            }
        }

        assert(near(sampleField(field, samplePosition(field, 2, 1, 3)).distance, field.at(2, 1, 3), 1e-6f));
        for (const Vec3& offset : {Vec3{0.1f, 0.2f, 0.3f}, Vec3{0.71f, 0.05f, 1.13f}, Vec3{0.99f, 0.74f, 0.01f}}) {
            const Vec3 point = field.origin + offset;
            const FieldPoint blended = sampleField(field, point);
            assert(near(blended.distance, slope.dot(point) + 0.1f, 1e-5f));
            assert(near(blended.gradient.x, slope.x, 1e-4f) && near(blended.gradient.y, slope.y, 1e-4f) && near(blended.gradient.z, slope.z, 1e-4f));
        }

        // The far faces of the box still blend (the last cell), and just past them is outside.
        const Vec3 corner = samplePosition(field, 4, 3, 5);
        assert(near(sampleField(field, corner).distance, field.at(4, 3, 5), 1e-6f));
        assert(sampleField(field, corner + Vec3{0.01f, 0.0f, 0.0f}).distance == field.band);
    }

    /*
    * The two steps a sample takes, which the GPU mirrors: the cell a point is in and its share across it (none outside
    * the box), then the blend of the cell's eight corners.
    */
    void testCellAndBlend() {
        SignedDistanceField field;
        field.origin = Vec3{1.0f, 2.0f, 3.0f};
        field.spacing = 0.5f;
        field.counts = {3, 4, 5};
        field.band = 1.0f;
        field.distances.assign(3 * 4 * 5, 0.25f);

        std::array<int, 3> cell{};
        Vec3 share;
        assert(fieldCell(field, Vec3{1.6f, 3.1f, 4.9f}, cell, share));
        assert(cell[0] == 1 && cell[1] == 2 && cell[2] == 3);
        assert(near(share.x, 0.2f, 1e-5f) && near(share.y, 0.2f, 1e-5f) && near(share.z, 0.8f, 1e-5f));

        // On the far faces, the last cell all the way across; past them, or before the first sample, none.
        assert(fieldCell(field, Vec3{2.0f, 3.5f, 5.0f}, cell, share));
        assert(cell[0] == 1 && cell[1] == 2 && cell[2] == 3 && share.x == 1.0f && share.y == 1.0f && share.z == 1.0f);
        assert(!fieldCell(field, Vec3{2.01f, 3.0f, 4.0f}, cell, share));
        assert(!fieldCell(field, Vec3{1.5f, 1.99f, 4.0f}, cell, share));

        // Equal corners blend to exactly their value with no slope; a corner raised by one slopes by one over the spacing.
        const FieldPoint flat = blendFieldCell({0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f}, Vec3{0.3f, 0.6f, 0.9f}, 0.5f);
        assert(flat.distance == 0.25f && flat.gradient.x == 0.0f && flat.gradient.y == 0.0f && flat.gradient.z == 0.0f);
        const FieldPoint raised = blendFieldCell({0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}, 0.5f);
        assert(raised.distance == 0.0f && near(raised.gradient.x, 2.0f, 1e-6f) && raised.gradient.y == 0.0f && raised.gradient.z == 0.0f);
    }

    // Each slab of the grid is a job of its own, so any number of threads gives the same field to the last bit.
    void testSameOnAnyThreads() {
        const TriangleMesh ball = collisionMesh(Mesh::sphere(32, 16), Mat4::scaling(0.2f, 0.2f, 0.2f));
        JobSystem jobs;
        const SignedDistanceField serial = buildSignedDistanceField(ball, spacing, band, serialJobs());
        const SignedDistanceField shared = buildSignedDistanceField(ball, spacing, band, jobs);
        assert(serial.distances == shared.distances);
    }

    void testBadFields() {
        const TriangleMesh cube = collisionMesh(Mesh::cube(), Mat4::identity());
        for (const auto& [badSpacing, badBand] : {std::pair{0.0f, 0.03f}, std::pair{-0.01f, 0.03f}, std::pair{0.01f, 0.005f}}) {
            bool threw = false;
            try {
                buildSignedDistanceField(cube, badSpacing, badBand);
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            assert(threw);
        }

        bool threw = false;
        try {
            buildSignedDistanceField(TriangleMesh{}, spacing, band);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }
}

void testSignedDistanceField() {
    testCubeSamples();
    testBallBetweenSamples();
    testOutsideTheBox();
    testBlending();
    testCellAndBlend();
    testSameOnAnyThreads();
    testBadFields();
}
