#include "assets/asset_manager.h"
#include "engine/game.h"
#include "render/gpu/shadow_cache.h"
#include "scene/deforming_mesh.h"
#include "scene/frame_description.h"

#include <bit>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    bool near(float actual, float expected, float tolerance = 1e-5f) {
        return std::abs(actual - expected) <= tolerance;
    }

    bool nearVec(const Vec3& actual, const Vec3& expected, float tolerance = 1e-5f) {
        return near(actual.x, expected.x, tolerance) && near(actual.y, expected.y, tolerance) && near(actual.z, expected.z, tolerance);
    }

    Vec3 xyz(const Vec4& v) {
        return Vec3{v.x, v.y, v.z};
    }

    std::vector<Vec3> restPositions(const Mesh& mesh) {
        std::vector<Vec3> positions;
        for (const Vec4& vertex : mesh.vertices) {
            positions.push_back(xyz(vertex));
        }
        return positions;
    }

    bool finiteUnit(const Vec3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && near(v.length(), 1.0f, 1e-4f);
    }

    // A sheet of columns x rows squares, width by height, centred on the origin and facing +z, its rows from the bottom.
    void testGrid() {
        const Mesh grid = Mesh::grid(4, 2, 2.0f, 1.0f);
        assert(grid.vertices.size() == 5 * 3);
        assert(grid.triangles.size() == 2 * 4 * 2);
        assert(nearVec(xyz(grid.vertices.front()), Vec3{-1.0f, -0.5f, 0.0f}));
        assert(nearVec(xyz(grid.vertices.back()), Vec3{1.0f, 0.5f, 0.0f}));

        for (const Triangle& triangle : grid.triangles) {
            const Vec3 normal = faceNormal(grid.vertices[triangle.first], grid.vertices[triangle.second], grid.vertices[triangle.third]);
            assert(nearVec(normal, Vec3{0.0f, 0.0f, 1.0f}));
            for (const std::optional<Vec2>& uv : triangle.uvs) {
                assert(uv.has_value());
                assert(uv->x >= 0.0f && uv->x <= 1.0f && uv->y >= 0.0f && uv->y <= 1.0f);
            }
        }

        // The bottom-left corner has UV (0, 0) and the top-right (1, 1), whichever triangles they are in.
        bool sawBottomLeft = false;
        bool sawTopRight = false;
        for (const Triangle& triangle : grid.triangles) {
            const std::size_t corners[3]{triangle.first, triangle.second, triangle.third};
            for (int corner = 0; corner < 3; ++corner) {
                if (corners[corner] == 0) {
                    assert(near(triangle.uvs[corner]->x, 0.0f) && near(triangle.uvs[corner]->y, 0.0f));
                    sawBottomLeft = true;
                }
                if (corners[corner] == grid.vertices.size() - 1) {
                    assert(near(triangle.uvs[corner]->x, 1.0f) && near(triangle.uvs[corner]->y, 1.0f));
                    sawTopRight = true;
                }
            }
        }
        assert(sawBottomLeft && sawTopRight);

        bool threw = false;
        try {
            Mesh::grid(0, 2, 1.0f, 1.0f);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    // A sheet's corners share a render vertex per source vertex (it has no seams), each knowing its source.
    void testTopologyOfASheet() {
        const Mesh grid = Mesh::grid(3, 3, 1.0f, 1.0f);
        const DeformingTopology topology = buildDeformingTopology(grid);
        assert(topology.sourceVertexCount == grid.vertices.size());
        assert(topology.rest.vertices.size() == grid.vertices.size());
        assert(topology.sourceVertex.size() == topology.rest.vertices.size());
        assert(topology.rest.indices.size() == grid.triangles.size() * 3);

        for (std::size_t vertex = 0; vertex < topology.rest.vertices.size(); ++vertex) {
            assert(nearVec(topology.rest.vertices[vertex].position, xyz(grid.vertices[topology.sourceVertex[vertex]])));
            assert(nearVec(topology.rest.vertices[vertex].normal, Vec3{0.0f, 0.0f, 1.0f}));
        }
    }

    // Deformed flat, a sheet keeps its normals; tilted, they follow; the tangent runs along increasing u, square to the
    // normal, keeping its side (w).
    void testNormalsAndTangentsFollowTheShape() {
        const Mesh grid = Mesh::grid(2, 2, 2.0f, 2.0f);
        const DeformingTopology topology = buildDeformingTopology(grid);
        std::vector<MeshVertex> vertices;

        deformVertices(topology, restPositions(grid), vertices);
        assert(vertices.size() == topology.rest.vertices.size());
        for (std::size_t vertex = 0; vertex < vertices.size(); ++vertex) {
            assert(nearVec(vertices[vertex].normal, Vec3{0.0f, 0.0f, 1.0f}));
            assert(nearVec(xyz(vertices[vertex].tangent), Vec3{1.0f, 0.0f, 0.0f}));
            assert(vertices[vertex].tangent.w == topology.rest.vertices[vertex].tangent.w);
            assert(vertices[vertex].uv.x == topology.rest.vertices[vertex].uv.x);
        }

        // Turned a quarter turn about y (x becomes -z): the sheet faces +x, its u runs along -z.
        std::vector<Vec3> turned;
        for (const Vec3& p : restPositions(grid)) {
            turned.push_back(Vec3{p.z, p.y, -p.x});
        }
        deformVertices(topology, turned, vertices);
        for (const MeshVertex& vertex : vertices) {
            assert(nearVec(vertex.normal, Vec3{1.0f, 0.0f, 0.0f}));
            assert(nearVec(xyz(vertex.tangent), Vec3{0.0f, 0.0f, -1.0f}));
        }

        // The middle vertex lifted towards the viewer: its own normal stays straight out (the bump is symmetric), its
        // neighbours lean away from it, and every tangent stays square to its normal.
        std::vector<Vec3> bumped = restPositions(grid);
        bumped[4].z = 0.5f;
        deformVertices(topology, bumped, vertices);
        for (std::size_t vertex = 0; vertex < vertices.size(); ++vertex) {
            const MeshVertex& v = vertices[vertex];
            assert(finiteUnit(v.normal));
            assert(finiteUnit(xyz(v.tangent)));
            assert(near(v.normal.dot(xyz(v.tangent)), 0.0f, 1e-4f));
            const std::uint32_t source = topology.sourceVertex[vertex];
            if (source == 4) {
                assert(nearVec(v.normal, Vec3{0.0f, 0.0f, 1.0f}));
            }
            if (source == 3) {
                assert(v.normal.x < 0.0f);
            }
            if (source == 5) {
                assert(v.normal.x > 0.0f);
            }
        }
    }

    // Normals are smooth across seams: a cube's corners, split by their faces' UVs, all get one normal, the area-weighted
    // average of their triangles' (a corner may lie in one or two triangles of a face), pointing out of the corner.
    void testNormalsAreSmoothAcrossSeams() {
        const Mesh cube = Mesh::cube();
        const DeformingTopology topology = buildDeformingTopology(cube);
        assert(topology.rest.vertices.size() > cube.vertices.size());

        std::vector<MeshVertex> vertices;
        deformVertices(topology, restPositions(cube), vertices);
        std::vector<std::optional<Vec3>> normalOfSource(cube.vertices.size());
        for (std::size_t vertex = 0; vertex < vertices.size(); ++vertex) {
            const std::uint32_t source = topology.sourceVertex[vertex];
            const Vec3 corner = xyz(cube.vertices[source]).normalized();
            assert(finiteUnit(vertices[vertex].normal));
            assert(vertices[vertex].normal.dot(corner) > 0.9f);
            if (normalOfSource[source]) {
                assert(nearVec(vertices[vertex].normal, *normalOfSource[source]));
            }
            normalOfSource[source] = vertices[vertex].normal;
        }
    }

    // A shape squashed to a point has no normals to find: they stay finite and of unit length, and so do the tangents.
    void testDegenerateShapesStayFinite() {
        const Mesh grid = Mesh::grid(2, 2, 1.0f, 1.0f);
        const DeformingTopology topology = buildDeformingTopology(grid);
        std::vector<MeshVertex> vertices;
        deformVertices(topology, std::vector<Vec3>(grid.vertices.size(), Vec3{1.0f, 2.0f, 3.0f}), vertices);
        for (const MeshVertex& vertex : vertices) {
            assert(finiteUnit(vertex.normal));
            assert(finiteUnit(xyz(vertex.tangent)));
        }

        bool threw = false;
        try {
            deformVertices(topology, std::vector<Vec3>(3), vertices);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    // Between ticks the shape is blended like a transform: alpha 0 is the last tick's, 1 the latest; without a last
    // tick's shape, the latest.
    void testShapesBlendBetweenTicks() {
        DeformingMesh mesh;
        mesh.positions = {Vec3{2.0f, 0.0f, 0.0f}, Vec3{0.0f, 4.0f, 0.0f}};
        assert(nearVec(blendedPositions(mesh, 0.3f)[1], Vec3{0.0f, 4.0f, 0.0f}));

        mesh.previousPositions = {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}};
        assert(nearVec(blendedPositions(mesh, 0.0f)[0], Vec3{0.0f, 0.0f, 0.0f}));
        assert(nearVec(blendedPositions(mesh, 1.0f)[0], Vec3{2.0f, 0.0f, 0.0f}));
        assert(nearVec(blendedPositions(mesh, 0.25f)[1], Vec3{0.0f, 1.0f, 0.0f}));
    }

    // Each tick starts by remembering the shape the last one left, as transforms are remembered.
    void testTicksRememberTheLastShape() {
        struct Shaper final : Game {
            Entity sheet;
            const char* title() const override { return "Shaper"; }
            void onInit(World&, AssetManager&) override {}
            void onInput(World&, const Input&) override {}
            void onFixedUpdate(World& world, float, double) override {
                world.get<DeformingMesh>(sheet).positions[0].x += 1.0f;
            }
            void onUpdate(World&, const Input&, float, float) override {}
            Camera& getCamera() override { return camera; }
            bool wantsMouseLook() const override { return false; }
            Camera camera{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.0f, 0.1f, 10.0f};
        };

        World world;
        Shaper game;
        game.sheet = world.create();
        DeformingMesh mesh;
        mesh.positions = {Vec3{0.0f, 0.0f, 0.0f}};
        world.add(game.sheet, mesh);

        runFixedUpdate(game, world, 1.0f / 120.0f, 0.0);
        runFixedUpdate(game, world, 1.0f / 120.0f, 0.0);
        const DeformingMesh& after = world.get<DeformingMesh>(game.sheet);
        assert(after.previousPositions.size() == 1);
        assert(near(after.previousPositions[0].x, 1.0f));
        assert(near(after.positions[0].x, 2.0f));
    }

    // A frame's deformed mesh: its vertices, its bounds and a fingerprint of its shape, the same for the same shape.
    void testDeformedMeshes() {
        const Mesh grid = Mesh::grid(2, 2, 2.0f, 2.0f);
        const DeformingTopology topology = buildDeformingTopology(grid);
        DeformingMesh mesh;
        mesh.mesh = MeshHandle{3};
        mesh.positions = restPositions(grid);
        mesh.positions[4].z = 0.75f;

        const DeformedMesh deformed = deformMesh(topology, mesh, 1.0f);
        assert(deformed.mesh.index == 3);
        assert(deformed.vertices.size() == topology.rest.vertices.size());
        assert(nearVec(deformed.bounds.minimum, Vec3{-1.0f, -1.0f, 0.0f}));
        assert(nearVec(deformed.bounds.maximum, Vec3{1.0f, 1.0f, 0.75f}));

        assert(deformMesh(topology, mesh, 1.0f).shape == deformed.shape);
        mesh.positions[4].z = 0.5f;
        assert(deformMesh(topology, mesh, 1.0f).shape != deformed.shape);
        assert(deformed.shape != 0);
    }

    // Draws of a deformed mesh carry its shape's fingerprint, and others none; the shadow cache sees it, so a tile is
    // drawn again when a caster changes shape though it stays put.
    void testShapesReachTheShadowCache() {
        std::vector<DrawItem> draws(3);
        draws[0].mesh = MeshHandle{1};
        draws[1].mesh = MeshHandle{2};
        draws[2].mesh = MeshHandle{1};
        DeformedMesh deformed;
        deformed.mesh = MeshHandle{1};
        deformed.shape = 1234;
        attachShapes(draws, {deformed});
        assert(draws[0].shape == 1234 && draws[2].shape == 1234);
        assert(draws[1].shape == 0);

        const std::vector<std::size_t> casters{0, 1};
        const std::uint64_t before = shadowCasterSignature(draws, casters);
        draws[0].shape = 5678;
        assert(shadowCasterSignature(draws, casters) != before);
    }

    // The asset manager keeps a deforming mesh's topology, built once; other meshes have none.
    void testAssetManagerKeepsTopologies() {
        AssetManager assets;
        const MeshHandle still = assets.addMesh(Mesh::cube());
        const MeshHandle sheet = assets.addDeformingMesh(Mesh::grid(3, 2, 1.0f, 1.0f));
        assert(!assets.isDeforming(still));
        assert(assets.isDeforming(sheet));
        assert(assets.getDeformingTopology(sheet).sourceVertexCount == 4 * 3);
        assert(assets.getMeshCount() == 2);

        bool threw = false;
        try {
            assets.getDeformingTopology(still);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    bool sameBits(const Vec3& a, const Vec3& b) {
        return std::bit_cast<std::uint32_t>(a.x) == std::bit_cast<std::uint32_t>(b.x) && std::bit_cast<std::uint32_t>(a.y) == std::bit_cast<std::uint32_t>(b.y) &&
               std::bit_cast<std::uint32_t>(a.z) == std::bit_cast<std::uint32_t>(b.z);
    }

    // The shaping's steps, shared by deformVertices and the GPU's shape pass (deform.glsl): each named and checked.
    void testShapingSteps() {
        const Vec3 normal = triangleAreaNormal(Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
        assert(nearVec(normal, Vec3{0.0f, 0.0f, 2.0f}));

        const TriangleTangent along = triangleTangent(Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
                                                      Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f});
        assert(along.hasUvArea && nearVec(along.alongU, Vec3{2.0f, 0.0f, 0.0f}));
        const TriangleTangent mirrored = triangleTangent(Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
                                                         Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{0.0f, -1.0f});
        assert(mirrored.hasUvArea && nearVec(mirrored.alongU, Vec3{2.0f, 0.0f, 0.0f}));
        const TriangleTangent flat = triangleTangent(Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
                                                     Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}, Vec2{2.0f, 2.0f});
        assert(!flat.hasUvArea);

        assert(nearVec(deformedNormal(Vec3{0.0f, 0.0f, 3.0f}), Vec3{0.0f, 0.0f, 1.0f}));
        assert(nearVec(deformedNormal(Vec3{}), Vec3{0.0f, 1.0f, 0.0f}));
        assert(nearVec(deformedTangent(Vec3{1.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, 1.0f}), Vec3{1.0f, 0.0f, 0.0f}));
        const Vec3 any = deformedTangent(Vec3{}, Vec3{0.0f, 0.0f, 1.0f});
        assert(finiteUnit(any) && near(any.z, 0.0f));
    }

    /*
    * Each Mesh vertex's triangles and each render vertex's, in the order deformVertices adds them up: a triangle once per
    * corner it has there. The cube has seams, so render vertices outnumber Mesh vertices.
    */
    void testAdjacency() {
        for (const Mesh& mesh : {Mesh::grid(3, 2, 1.0f, 1.0f), Mesh::cube()}) {
            const DeformingTopology topology = buildDeformingTopology(mesh);
            const DeformingAdjacency adjacency = buildDeformingAdjacency(topology);
            assert(adjacency.sourceStarts.size() == topology.sourceVertexCount + 1);
            assert(adjacency.vertexStarts.size() == topology.rest.vertices.size() + 1);
            assert(adjacency.sourceTriangles.size() == topology.rest.indices.size());
            assert(adjacency.vertexTriangles.size() == topology.rest.indices.size());

            const std::size_t triangles = topology.rest.indices.size() / 3;
            for (std::uint32_t source = 0; source < topology.sourceVertexCount; ++source) {
                std::vector<std::uint32_t> expected;
                for (std::uint32_t triangle = 0; triangle < triangles; ++triangle) {
                    for (int corner = 0; corner < 3; ++corner) {
                        if (topology.sourceVertex[topology.rest.indices[triangle * 3 + corner]] == source) {
                            expected.push_back(triangle);
                        }
                    }
                }
                const std::vector<std::uint32_t> listed(adjacency.sourceTriangles.begin() + adjacency.sourceStarts[source],
                                                        adjacency.sourceTriangles.begin() + adjacency.sourceStarts[source + 1]);
                assert(listed == expected);
            }
            for (std::uint32_t vertex = 0; vertex < topology.rest.vertices.size(); ++vertex) {
                for (std::uint32_t index = adjacency.vertexStarts[vertex]; index < adjacency.vertexStarts[vertex + 1]; ++index) {
                    const std::uint32_t triangle = adjacency.vertexTriangles[index];
                    const std::uint32_t* corners = &topology.rest.indices[triangle * 3];
                    assert(corners[0] == vertex || corners[1] == vertex || corners[2] == vertex);
                }
            }
        }
    }

    /*
    * The shape pass's way, one render vertex at a time gathering its own triangles, gives deformVertices' result to the
    * bit, since it adds the same numbers in the same order: on a bumped sheet and a squashed cube.
    */
    void testGatheredVerticesMatch() {
        const Mesh grid = Mesh::grid(6, 4, 1.0f, 1.0f);
        std::vector<Vec3> bumped = restPositions(grid);
        for (std::size_t index = 0; index < bumped.size(); ++index) {
            bumped[index].z = 0.1f * std::sin(static_cast<float>(index) * 0.7f);
        }
        std::vector<Vec3> squashed = restPositions(Mesh::cube());
        for (Vec3& position : squashed) {
            position.y *= 0.0f;
        }

        const std::pair<Mesh, std::vector<Vec3>> cases[]{{grid, bumped}, {Mesh::cube(), squashed}};
        for (const auto& [mesh, positions] : cases) {
            const DeformingTopology topology = buildDeformingTopology(mesh);
            const DeformingAdjacency adjacency = buildDeformingAdjacency(topology);
            std::vector<MeshVertex> expected;
            deformVertices(topology, positions, expected);
            for (std::uint32_t vertex = 0; vertex < expected.size(); ++vertex) {
                const MeshVertex gathered = deformVertexAt(topology, adjacency, positions, vertex);
                assert(sameBits(gathered.position, expected[vertex].position));
                assert(sameBits(gathered.normal, expected[vertex].normal));
                assert(sameBits(xyz(gathered.tangent), xyz(expected[vertex].tangent)));
                assert(gathered.tangent.w == expected[vertex].tangent.w);
                assert(gathered.uv.x == expected[vertex].uv.x && gathered.uv.y == expected[vertex].uv.y);
            }
        }
    }
}

void testDeformingMesh() {
    testGrid();
    testTopologyOfASheet();
    testNormalsAndTangentsFollowTheShape();
    testNormalsAreSmoothAcrossSeams();
    testDegenerateShapesStayFinite();
    testShapesBlendBetweenTicks();
    testTicksRememberTheLastShape();
    testDeformedMeshes();
    testShapesReachTheShadowCache();
    testAssetManagerKeepsTopologies();
    testShapingSteps();
    testAdjacency();
    testGatheredVerticesMatch();
}
