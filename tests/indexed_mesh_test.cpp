#include "assets/gltf_loader.h"
#include "assets/obj_loader.h"
#include "scene/indexed_mesh.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
    bool nearlyEqual(const Vec3& a, const Vec3& b) {
        return std::abs(a.x - b.x) < 0.00001f &&
               std::abs(a.y - b.y) < 0.00001f &&
               std::abs(a.z - b.z) < 0.00001f;
    }

    template <typename Function>
    bool throws(Function function) {
        try {
            function();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    }

    // Indexing must never change the shape: every triangle rebuilt from indices has the original corner positions.
    void assertSameTriangles(const Mesh& mesh, const IndexedMesh& indexed) {
        assert(indexed.indices.size() == mesh.triangles.size() * 3);

        for (std::size_t triangle = 0; triangle < mesh.triangles.size(); ++triangle) {
            const Triangle& source = mesh.triangles[triangle];
            const std::size_t corners[3] = {source.first, source.second, source.third};

            for (std::size_t corner = 0; corner < 3; ++corner) {
                const Vec4& original = mesh.vertices[corners[corner]];
                const MeshVertex& vertex = indexed.vertices.at(indexed.indices[triangle * 3 + corner]);
                assert(nearlyEqual(vertex.position, Vec3{original.x, original.y, original.z}));
            }
        }
    }
}

void testIndexedMesh() {
    // A cube: 8 positions, but each corner touches 3 faces with different normals, so 6 faces x 4 corners = 24 vertices.
    // The two triangles of each face share their diagonal, so 36 corners become 24 vertices.
    {
        const Mesh cube = Mesh::cube();
        const IndexedMesh indexed = buildIndexedMesh(cube);

        assert(indexed.vertices.size() == 24);
        assert(indexed.indices.size() == 36);
        assertSameTriangles(cube, indexed);

        // The cube has no normals in the file, so each corner gets its face's normal. The first face points down -Z.
        assert(nearlyEqual(indexed.vertices[indexed.indices[0]].normal, Vec3{0.0f, 0.0f, -1.0f}));
    }

    // A plane: two triangles sharing two corners, so 4 vertices.
    {
        const Mesh plane = Mesh::plane(1.0f);
        const IndexedMesh indexed = buildIndexedMesh(plane);

        assert(indexed.vertices.size() == 4);
        assert(indexed.indices.size() == 6);
        assertSameTriangles(plane, indexed);
        assert(nearlyEqual(indexed.vertices[0].normal, Vec3{0.0f, 1.0f, 0.0f}));
    }

    // Corners at the same position stay separate when anything else differs, such as the UV along a texture seam.
    {
        Mesh seam;
        seam.vertices = {
            Vec4{0.0f, 0.0f, 0.0f, 1.0f},
            Vec4{1.0f, 0.0f, 0.0f, 1.0f},
            Vec4{0.0f, 1.0f, 0.0f, 1.0f},
            Vec4{1.0f, 1.0f, 0.0f, 1.0f}
        };

        Triangle first{0, 1, 2};
        Triangle second{1, 3, 2};
        first.uvs = {Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f}};
        second.uvs = {Vec2{0.5f, 0.0f}, Vec2{1.0f, 1.0f}, Vec2{0.0f, 1.0f}};
        seam.triangles = {first, second};

        const IndexedMesh indexed = buildIndexedMesh(seam);

        // Corner 2 is identical in both triangles and shared; corner 1 has two different UVs and is not.
        assert(indexed.vertices.size() == 5);
        assert(indexed.indices[2] == indexed.indices[5]);
        assert(indexed.indices[1] != indexed.indices[3]);

        // A normal given in the file wins over the face normal.
        Mesh custom = seam;
        custom.triangles[0].normals[0] = Vec3{0.0f, 1.0f, 0.0f};
        const IndexedMesh withNormal = buildIndexedMesh(custom);
        assert(nearlyEqual(withNormal.vertices[withNormal.indices[0]].normal, Vec3{0.0f, 1.0f, 0.0f}));
        assert(nearlyEqual(withNormal.vertices[withNormal.indices[1]].normal, Vec3{0.0f, 0.0f, 1.0f}));
    }

    // A model with smooth normals in the file shares most corners between several triangles.
    {
        const Model duck = loadGltf("assets/models/Duck.glb");
        std::size_t corners = 0;
        std::size_t vertices = 0;

        for (const ModelPart& part : duck.parts) {
            const IndexedMesh indexed = buildIndexedMesh(*part.mesh);
            assertSameTriangles(*part.mesh, indexed);
            corners += indexed.indices.size();
            vertices += indexed.vertices.size();
        }

        // Each vertex is used by several triangles on average.
        assert(vertices * 3 < corners);
    }

    // The teapot file has no normals, so every corner takes its own triangle's face normal (flat shading).
    // Corners only merge when their triangles lie in exactly the same plane: indexing never changes how a model looks.
    {
        const Mesh teapot = loadObj("assets/models/teapot.obj");
        const IndexedMesh indexed = buildIndexedMesh(teapot);

        assertSameTriangles(teapot, indexed);
        assert(indexed.vertices.size() <= teapot.triangles.size() * 3);
    }

    // Bad meshes are rejected before anything reaches the GPU.
    {
        assert(throws([] { buildIndexedMesh(Mesh{}); }));

        Mesh missingVertex = Mesh::plane(1.0f);
        missingVertex.triangles[0].third = 99;
        assert(throws([&] { buildIndexedMesh(missingVertex); }));

        Mesh notAPoint = Mesh::plane(1.0f);
        notAPoint.vertices[0].w = 0.0f;
        assert(throws([&] { buildIndexedMesh(notAPoint); }));

        Mesh notFinite = Mesh::plane(1.0f);
        notFinite.vertices[0].x = std::numeric_limits<float>::infinity();
        assert(throws([&] { buildIndexedMesh(notFinite); }));
    }
}
