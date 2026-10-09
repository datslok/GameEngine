#include "scene/mesh.h"
#include "scene/smooth_normals.h"

#include <cassert>
#include <cmath>
#include <numbers>

namespace {
    constexpr float degrees = std::numbers::pi_v<float> / 180.0f;

    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }

    bool nearlyEqual(const Vec3& actual, const Vec3& expected) {
        return nearlyEqual(actual.x, expected.x) && nearlyEqual(actual.y, expected.y) && nearlyEqual(actual.z, expected.z);
    }

    Vec3 normalOf(const Mesh& mesh, std::size_t triangle, int corner) {
        const std::optional<Vec3>& normal = mesh.triangles[triangle].normals[corner];
        assert(normal.has_value());
        return *normal;
    }

    std::size_t vertexOf(const Triangle& triangle, int corner) {
        const std::size_t indices[3] = {triangle.first, triangle.second, triangle.third};
        return indices[corner];
    }

    // Each slope of the roof tilts 10 degrees from flat, so the two faces meet at 20 degrees along the ridge at x = 0.
    const float roofDrop = std::tan(10.0f * degrees);
    const Vec3 leftSlope = Vec3{-roofDrop, 1.0f, 0.0f}.normalized();
    const Vec3 rightSlope = Vec3{roofDrop, 1.0f, 0.0f}.normalized();

    /*
    * Two slopes of a shallow roof sharing a ridge. With duplicateRidge, the right slope uses its own copies of the
    * ridge vertices (same positions, different indices), the way some OBJ files split surfaces along seams.
    */
    Mesh makeRoof(bool duplicateRidge) {
        Mesh mesh;
        mesh.vertices = {
            Vec4{-1.0f, -roofDrop, 0.0f, 1.0f},  // 0: left eave, front
            Vec4{-1.0f, -roofDrop, -1.0f, 1.0f}, // 1: left eave, back
            Vec4{0.0f, 0.0f, 0.0f, 1.0f},        // 2: ridge, front
            Vec4{0.0f, 0.0f, -1.0f, 1.0f},       // 3: ridge, back
            Vec4{1.0f, -roofDrop, 0.0f, 1.0f},   // 4: right eave, front
            Vec4{1.0f, -roofDrop, -1.0f, 1.0f},  // 5: right eave, back
            Vec4{0.0f, 0.0f, 0.0f, 1.0f},        // 6: copy of the front ridge
            Vec4{0.0f, 0.0f, -1.0f, 1.0f}        // 7: copy of the back ridge
        };

        const std::size_t ridgeFront = duplicateRidge ? 6 : 2;
        const std::size_t ridgeBack = duplicateRidge ? 7 : 3;

        mesh.triangles = {
            Triangle{0, 2, 3},
            Triangle{0, 3, 1},
            Triangle{ridgeFront, 4, 5},
            Triangle{ridgeFront, 5, ridgeBack}
        };

        return mesh;
    }

    // Corners on the ridge get the average of both slopes; corners on the eaves touch one slope only.
    void assertRoofIsSmooth(const Mesh& mesh) {
        for (std::size_t triangle = 0; triangle < mesh.triangles.size(); ++triangle) {
            for (int corner = 0; corner < 3; ++corner) {
                const float x = mesh.vertices[vertexOf(mesh.triangles[triangle], corner)].x;
                const Vec3 expected = x == 0.0f ? Vec3{0.0f, 1.0f, 0.0f} : (x < 0.0f ? leftSlope : rightSlope);
                assert(nearlyEqual(normalOf(mesh, triangle, corner), expected));
            }
        }
    }

    void testFlatQuadKeepsPlaneNormal() {
        Mesh mesh;
        mesh.vertices = {
            Vec4{0.0f, 0.0f, 0.0f, 1.0f},
            Vec4{1.0f, 0.0f, 0.0f, 1.0f},
            Vec4{1.0f, 0.0f, -1.0f, 1.0f},
            Vec4{0.0f, 0.0f, -1.0f, 1.0f}
        };
        mesh.triangles = {Triangle{0, 1, 2}, Triangle{0, 2, 3}};

        generateSmoothNormals(mesh, 60.0f * degrees);

        for (std::size_t triangle = 0; triangle < 2; ++triangle) {
            for (int corner = 0; corner < 3; ++corner) {
                assert(nearlyEqual(normalOf(mesh, triangle, corner), Vec3{0.0f, 1.0f, 0.0f}));
            }
        }
    }

    void testShallowRoofIsAveraged() {
        Mesh mesh = makeRoof(false);
        generateSmoothNormals(mesh, 60.0f * degrees);
        assertRoofIsSmooth(mesh);
    }

    // A crease angle smaller than the angle between the faces keeps the ridge sharp: the stealth fighter case.
    void testSmallCreaseAngleKeepsRidgeSharp() {
        Mesh mesh = makeRoof(false);
        generateSmoothNormals(mesh, 10.0f * degrees);

        for (int corner = 0; corner < 3; ++corner) {
            assert(nearlyEqual(normalOf(mesh, 0, corner), leftSlope));
            assert(nearlyEqual(normalOf(mesh, 2, corner), rightSlope));
        }
    }

    // Seams in the file must not show: corners are grouped by position, not by vertex index.
    void testDuplicatedVerticesAreSmoothedTogether() {
        Mesh mesh = makeRoof(true);
        generateSmoothNormals(mesh, 60.0f * degrees);
        assertRoofIsSmooth(mesh);
    }

    // Faces meeting at 90 degrees are past a 60 degree crease, so every cube corner keeps its face's normal.
    void testCubeStaysSharp() {
        Mesh mesh = Mesh::cube();
        generateSmoothNormals(mesh, 60.0f * degrees);

        for (std::size_t triangle = 0; triangle < mesh.triangles.size(); ++triangle) {
            const Triangle& face = mesh.triangles[triangle];
            const Vec4& a = mesh.vertices[face.first];
            const Vec4& b = mesh.vertices[face.second];
            const Vec4& c = mesh.vertices[face.third];
            const Vec3 expected = Vec3{b.x - a.x, b.y - a.y, b.z - a.z}.cross(Vec3{c.x - a.x, c.y - a.y, c.z - a.z}).normalized();

            for (int corner = 0; corner < 3; ++corner) {
                assert(nearlyEqual(normalOf(mesh, triangle, corner), expected));
            }
        }
    }

    // Normals that came from the file are what the artist wanted, so they are never replaced.
    void testFileNormalsAreKept() {
        Mesh mesh = makeRoof(false);
        mesh.triangles[0].normals[1] = Vec3{1.0f, 0.0f, 0.0f};

        generateSmoothNormals(mesh, 60.0f * degrees);

        assert(nearlyEqual(normalOf(mesh, 0, 1), Vec3{1.0f, 0.0f, 0.0f}));
    }

    // A triangle with no area has no direction: it must not add NaNs to its neighbours or get a normal of its own.
    void testDegenerateTriangleIsIgnored() {
        Mesh mesh = makeRoof(false);
        mesh.triangles.push_back(Triangle{2, 2, 3});

        generateSmoothNormals(mesh, 60.0f * degrees);

        for (int corner = 0; corner < 3; ++corner) {
            assert(!mesh.triangles.back().normals[corner].has_value());
        }

        mesh.triangles.pop_back();
        assertRoofIsSmooth(mesh);
    }
}

void testSmoothNormals() {
    testFlatQuadKeepsPlaneNormal();
    testShallowRoofIsAveraged();
    testSmallCreaseAngleKeepsRidgeSharp();
    testDuplicatedVerticesAreSmoothedTogether();
    testCubeStaysSharp();
    testFileNormalsAreKept();
    testDegenerateTriangleIsIgnored();
}
