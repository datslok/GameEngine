#include "assets/gltf_loader.h"
#include "assets/obj_loader.h"
#include "scene/indexed_mesh.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance = 0.0001f) {
        return std::abs(actual - expected) < tolerance;
    }

    bool nearlyEqual(const Vec3& actual, const Vec3& expected, float tolerance = 0.0001f) {
        return nearlyEqual(actual.x, expected.x, tolerance) &&
               nearlyEqual(actual.y, expected.y, tolerance) &&
               nearlyEqual(actual.z, expected.z, tolerance);
    }

    Vec3 direction(const Vec4& tangent) {
        return Vec3{tangent.x, tangent.y, tangent.z};
    }

    // The bitangent the shader rebuilds: the tangent's sign times normal x tangent.
    Vec3 bitangentOf(const MeshVertex& vertex) {
        return vertex.normal.cross(direction(vertex.tangent)) * vertex.tangent.w;
    }

    /*
    * A unit square in the XY plane facing +Z (counterclockwise seen from the front), as two triangles, with each corner's
    * UV computed from its position. Every test below describes the same picture laid on the square in a different way.
    */
    Mesh square(const std::function<Vec2(float x, float y)>& uvAt, UvOrigin origin) {
        Mesh mesh;
        mesh.vertices = {
            Vec4{0.0f, 0.0f, 0.0f, 1.0f},
            Vec4{1.0f, 0.0f, 0.0f, 1.0f},
            Vec4{1.0f, 1.0f, 0.0f, 1.0f},
            Vec4{0.0f, 1.0f, 0.0f, 1.0f}
        };
        mesh.uvOrigin = origin;

        Triangle first{0, 1, 2};
        Triangle second{0, 2, 3};

        for (Triangle* triangle : {&first, &second}) {
            const std::size_t corners[3] = {triangle->first, triangle->second, triangle->third};

            for (std::size_t corner = 0; corner < 3; ++corner) {
                const Vec4& position = mesh.vertices[corners[corner]];
                triangle->uvs[corner] = uvAt(position.x, position.y);
                triangle->normals[corner] = Vec3{0.0f, 0.0f, 1.0f};
            }
        }

        mesh.triangles = {first, second};
        return mesh;
    }

    void assertEveryCorner(const IndexedMesh& mesh, const Vec3& tangent, const Vec3& bitangent) {
        for (const MeshVertex& vertex : mesh.vertices) {
            assert(nearlyEqual(direction(vertex.tangent), tangent));
            assert(vertex.tangent.w == 1.0f || vertex.tangent.w == -1.0f);
            assert(nearlyEqual(bitangentOf(vertex), bitangent));
        }
    }

    // The textbook case: u runs along +x and v up +y (OBJ's bottom-left origin), so the frame is x, y, z itself.
    void testUvAxesBecomeTheFrame() {
        const IndexedMesh mesh = buildIndexedMesh(square([](float x, float y) { return Vec2{x, y}; }, UvOrigin::BottomLeft));

        assertEveryCorner(mesh, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
        assert(mesh.vertices[0].tangent.w == 1.0f);
    }

    /*
    * glTF counts v down from the image's top row. The same picture, the right way up on the square, has v = 1 - y there,
    * and must get the same frame: the bitangent is "up in the image", which is what a normal map's green channel means.
    */
    void testTopLeftOriginDescribesTheSameImage() {
        const IndexedMesh mesh = buildIndexedMesh(square([](float x, float y) { return Vec2{x, 1.0f - y}; }, UvOrigin::TopLeft));

        assertEveryCorner(mesh, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    }

    // The same UV numbers read with the other origin put the picture upside down, so its "up" is -y.
    void testOriginDecidesWhichWayIsUp() {
        const IndexedMesh mesh = buildIndexedMesh(square([](float x, float y) { return Vec2{x, y}; }, UvOrigin::TopLeft));

        assertEveryCorner(mesh, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f});
        assert(mesh.vertices[0].tangent.w == -1.0f);
    }

    /*
    * A mirrored UV island: u runs along -x. The tangent follows u, and the bitangent must still point up the image (+y),
    * which normal x tangent no longer does, so the sign flips to -1.
    */
    void testMirroredUvsFlipTheSign() {
        const IndexedMesh mesh = buildIndexedMesh(square([](float x, float y) { return Vec2{1.0f - x, y}; }, UvOrigin::BottomLeft));

        assertEveryCorner(mesh, Vec3{-1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
        assert(mesh.vertices[0].tangent.w == -1.0f);
    }

    // A picture turned a quarter turn: u runs up +y and v along -x. The frame turns with it and keeps its handedness.
    void testTurnedUvsTurnTheFrame() {
        const IndexedMesh mesh = buildIndexedMesh(square([](float x, float y) { return Vec2{y, -x}; }, UvOrigin::BottomLeft));

        assertEveryCorner(mesh, Vec3{0.0f, 1.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f});
        assert(mesh.vertices[0].tangent.w == 1.0f);
    }

    // A tangent stored in the file is the one its normal map was baked against, so it is kept, not regenerated.
    void testFileTangentsAreKept() {
        Mesh mesh = square([](float x, float y) { return Vec2{x, y}; }, UvOrigin::BottomLeft);

        for (Triangle& triangle : mesh.triangles) {
            triangle.tangents.fill(Vec4{0.0f, 1.0f, 0.0f, -1.0f});
        }

        const IndexedMesh indexed = buildIndexedMesh(mesh);
        assertEveryCorner(indexed, Vec3{0.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f});
    }

    /*
    * Two squares side by side, the right one's picture mirrored (u = 2 - x), as artists do to paint half a face once. The
    * corners on the shared edge agree in position, normal and UV, but not in tangent, so they must stay two vertices:
    * merging them would average two opposite frames into nonsense along the seam.
    */
    void testMirrorSeamKeepsTwoVertices() {
        Mesh left = square([](float x, float y) { return Vec2{x, y}; }, UvOrigin::BottomLeft);
        Mesh right = square([](float x, float y) { return Vec2{1.0f - x, y}; }, UvOrigin::BottomLeft);

        Mesh both = left;
        const std::size_t offset = both.vertices.size();

        for (const Vec4& vertex : right.vertices) {
            both.vertices.push_back(Vec4{vertex.x + 1.0f, vertex.y, vertex.z, 1.0f});
        }

        for (Triangle triangle : right.triangles) {
            triangle.first += offset;
            triangle.second += offset;
            triangle.third += offset;
            both.triangles.push_back(triangle);
        }

        const IndexedMesh indexed = buildIndexedMesh(both);
        assert(indexed.vertices.size() == 8);

        // Each half keeps its own frame, and both bitangents point up the picture.
        for (const MeshVertex& vertex : indexed.vertices) {
            assert(nearlyEqual(bitangentOf(vertex), Vec3{0.0f, 1.0f, 0.0f}));
        }
    }

    // Without UVs there is no picture to follow, but the shader still needs a frame: any unit tangent across the normal.
    void testMissingUvsStillGiveAFrame() {
        const Mesh teapot = loadObj("assets/models/teapot.obj");
        const IndexedMesh indexed = buildIndexedMesh(teapot);

        for (const MeshVertex& vertex : indexed.vertices) {
            const Vec3 tangent = direction(vertex.tangent);
            assert(std::isfinite(tangent.x) && std::isfinite(tangent.y) && std::isfinite(tangent.z));
            assert(nearlyEqual(tangent.length(), 1.0f, 0.001f));
            assert(std::abs(tangent.dot(vertex.normal)) < 0.001f);
            assert(vertex.tangent.w == 1.0f || vertex.tangent.w == -1.0f);
        }
    }

    /*
    * The test model's tangents were made by Blender with MikkTSpace. Throwing them away and generating our own from the
    * same positions, normals and UVs must give the same frames, signs included: this checks the whole convention (which
    * way v runs in glTF, which way the bitangent points) against a real exporter rather than against our own reasoning.
    */
    void testGeneratedTangentsMatchTheExporter() {
        const Model model = loadGltf("assets/models/NormalTangentMirrorTest.glb");
        std::size_t corners = 0;
        std::size_t matching = 0;
        std::size_t mirrored = 0;

        for (const ModelPart& part : model.parts) {
            Mesh withoutTangents = *part.mesh;

            for (Triangle& triangle : withoutTangents.triangles) {
                triangle.tangents = {};
            }

            const std::vector<MeshVertex> supplied = resolveCorners(*part.mesh);
            const std::vector<MeshVertex> generated = resolveCorners(withoutTangents);
            assert(supplied.size() == generated.size());

            for (std::size_t corner = 0; corner < supplied.size(); ++corner) {
                ++corners;
                mirrored += supplied[corner].tangent.w < 0.0f ? 1 : 0;

                if (supplied[corner].tangent.w == generated[corner].tangent.w &&
                    direction(supplied[corner].tangent).dot(direction(generated[corner].tangent)) > 0.99f) {
                    ++matching;
                }
            }
        }

        // The model is about mirroring, so both signs occur; nearly every corner agrees (exporters round their output).
        assert(corners > 0);
        assert(mirrored > 0 && mirrored < corners);
        assert(matching * 100 >= corners * 99);
    }
}

void testTangents() {
    testUvAxesBecomeTheFrame();
    testTopLeftOriginDescribesTheSameImage();
    testOriginDecidesWhichWayIsUp();
    testMirroredUvsFlipTheSign();
    testTurnedUvsTurnTheFrame();
    testFileTangentsAreKept();
    testMirrorSeamKeepsTwoVertices();
    testMissingUvsStillGiveAFrame();
    testGeneratedTangentsMatchTheExporter();
}
