#include "render/gpu/gpu_scene.h"
#include "render/gpu/ray_tracing.h"
#include "render/gpu/terrain_gpu.h"
#include "scene/culling.h"
#include "scene/terrain.h"
#include "test_jobs.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    // A tilted plane: bilinear sampling of any grid of it is exact anywhere.
    float plane(float x, float z) {
        return 0.5f * x + 0.25f * z + 3.0f;
    }

    // A wavy land, to test that sampling picks the finest grid holding a place.
    float waves(float x, float z) {
        return 4.0f * std::sin(0.05f * x) * std::cos(0.03f * z);
    }

    // A fine grid 64 m across round the origin at 1 m, a coarse one 1 km across at 16 m.
    std::shared_ptr<const TerrainHeights> twoGrids(float (*height)(float, float)) {
        auto heights = std::make_shared<TerrainHeights>();
        heights->grids.push_back(makeTerrainHeightGrid(Vec2{0.0f, 0.0f}, 32.0f, 1.0f, height, testJobs()));
        heights->grids.push_back(makeTerrainHeightGrid(Vec2{0.0f, 0.0f}, 512.0f, 16.0f, height, testJobs()));
        return heights;
    }

    /*
    * A grid holds the land's heights at its points, corner first, rows along x: size points a side from centre - halfSize.
    * Sampling is bilinear in the finest grid holding the place, so a plane comes back exactly anywhere, and a place
    * outside every grid reads the coarsest's nearest edge.
    */
    void testHeightsAreSampledFromTheFinestGrid() {
        const std::shared_ptr<const TerrainHeights> flat = twoGrids(plane);
        const TerrainHeightGrid& fine = flat->grids[0];
        assert(fine.size == 65 && fine.spacing == 1.0f && fine.corner.x == -32.0f && fine.corner.y == -32.0f);
        assert(fine.heights.size() == 65u * 65u && fine.heights[1] == plane(-31.0f, -32.0f) && fine.heights[65] == plane(-32.0f, -31.0f));

        assert(near(terrainHeight(*flat, 3.3f, -7.9f), plane(3.3f, -7.9f), 1e-4f));
        assert(near(terrainHeight(*flat, 300.5f, 100.25f), plane(300.5f, 100.25f), 1e-3f));

        const std::shared_ptr<const TerrainHeights> wavy = twoGrids(waves);
        assert(near(terrainHeight(*wavy, 10.0f, 5.0f), waves(10.0f, 5.0f), 1e-5f));
        assert(terrainHeight(*wavy, 2000.0f, 0.0f) == terrainHeight(*wavy, 512.0f, 0.0f));

        // The normal from the slope: a plane's, everywhere.
        const Vec3 normal = terrainNormal(*flat, 5.0f, 5.0f, 2.0f);
        const Vec3 expected = Vec3{-0.5f, 1.0f, -0.25f}.normalized();
        assert(near(normal.x, expected.x, 1e-4f) && near(normal.y, expected.y, 1e-4f) && near(normal.z, expected.z, 1e-4f));
    }

    /*
    * The drawn surface: each grid square split into two flat triangles as the grid mesh splits it (the diagonal from
    * its near right corner to its far left), so a height on it is the plane through that triangle's corners, not the
    * bilinear one; on a grid point both are the grid's height, and the two triangles agree along their diagonal.
    */
    void testTheDrawnSurfaceIsTheGridsTriangles() {
        const Mesh quad = terrainGridMesh(1);
        assert(quad.triangles.size() == 2);
        const float heights[2][2] = {{1.0f, 4.0f}, {2.0f, -3.0f}}; // [row][column]: near left, near right; far left, far right

        for (const Triangle& triangle : quad.triangles) {
            const std::size_t corners[3] = {triangle.first, triangle.second, triangle.third};
            Vec3 points[3];

            for (int corner = 0; corner < 3; ++corner) {
                const Vec4& vertex = quad.vertices[corners[corner]];
                points[corner] = Vec3{vertex.x, heights[static_cast<int>(vertex.z)][static_cast<int>(vertex.x)], vertex.z};
            }

            // The triangle's middle: its plane's height there is the corners' mean.
            const float across = (points[0].x + points[1].x + points[2].x) / 3.0f;
            const float down = (points[0].z + points[1].z + points[2].z) / 3.0f;
            const float expected = (points[0].y + points[1].y + points[2].y) / 3.0f;
            assert(near(terrainTriangleHeight(heights[0][0], heights[0][1], heights[1][0], heights[1][1], across, down), expected, 1e-5f));
        }

        // Along the diagonal both triangles agree; at the corners, the corners' heights.
        assert(near(terrainTriangleHeight(1.0f, 4.0f, 2.0f, -3.0f, 0.3f, 0.7f), 1.0f + 0.3f * 3.0f + 0.7f * 1.0f, 1e-5f));
        assert(terrainTriangleHeight(1.0f, 4.0f, 2.0f, -3.0f, 1.0f, 1.0f) == -3.0f);

        // In a grid, the drawn surface reads the finest grid's triangles: on a plane it is the plane, at a point the grid's.
        const std::shared_ptr<const TerrainHeights> tilted = twoGrids(plane);
        assert(near(terrainSurfaceHeight(*tilted, 3.3f, -7.9f), plane(3.3f, -7.9f), 1e-4f));
        const std::shared_ptr<const TerrainHeights> wavy = twoGrids(waves);
        assert(terrainSurfaceHeight(*wavy, 5.0f, -3.0f) == terrainHeight(*wavy, 5.0f, -3.0f));
    }

    /*
    * CDLOD's morph (Strugar 2010): 0 before the morph's start, 1 from its end, linear between. A grid point at an odd
    * place slides towards its even neighbour below as the morph grows, reaching it at 1, so a patch fully morphed is the
    * next coarser level's grid exactly; even points stay.
    */
    void testTheMorphSlidesOddPointsOntoEvenOnes() {
        assert(terrainMorph(10.0f, 20.0f, 30.0f) == 0.0f && terrainMorph(25.0f, 20.0f, 30.0f) == 0.5f && terrainMorph(40.0f, 20.0f, 30.0f) == 1.0f);

        const Vec2 even = morphGridCoordinate(Vec2{4.0f, 6.0f}, 1.0f);
        assert(even.x == 4.0f && even.y == 6.0f);
        const Vec2 odd = morphGridCoordinate(Vec2{5.0f, 7.0f}, 1.0f);
        assert(odd.x == 4.0f && odd.y == 6.0f);
        const Vec2 half = morphGridCoordinate(Vec2{5.0f, 6.0f}, 0.5f);
        assert(half.x == 4.5f && half.y == 6.0f);
    }

    // A flat land 4 km across round the origin, leaves of 64 m in 6 levels (the root 2 km), ranges from 100 m.
    Terrain flatTerrain() {
        auto heights = std::make_shared<TerrainHeights>();
        heights->grids.push_back(makeTerrainHeightGrid(Vec2{0.0f, 0.0f}, 2048.0f, 32.0f, [](float, float) { return 0.0f; }, testJobs()));
        Terrain terrain;
        terrain.heights = heights;
        terrain.centre = Vec2{0.0f, 0.0f};
        terrain.leafSize = 64.0f;
        terrain.levels = 6;
        terrain.landHalfSize = 1000.0f;
        terrain.bounds = std::make_shared<const TerrainBounds>(makeTerrainBounds(terrain, testJobs()));
        terrain.grid = MeshHandle{1};
        terrain.halfGrid = MeshHandle{2};
        terrain.regions = {TerrainRegion{.centre = Vec2{0.0f, 0.0f}, .halfSize = 128.0f, .material = Material{}},
                           TerrainRegion{.centre = Vec2{0.0f, 0.0f}, .halfSize = 1024.0f, .material = Material{}}};
        terrain.regions[0].material.roughness = 0.25f;
        terrain.regions[1].material.roughness = 0.75f;
        return terrain;
    }

    const TerrainSettings ranges{.firstRangeMetres = 100.0f, .morphShare = 0.3f};

    // The node holding a place, its index in nodes (or none).
    std::vector<std::size_t> nodesHolding(const std::vector<TerrainNode>& nodes, float x, float z) {
        std::vector<std::size_t> holding;

        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const TerrainNode& node = nodes[index];

            if (x >= node.corner.x && x < node.corner.x + node.size && z >= node.corner.y && z < node.corner.y + node.size) {
                holding.push_back(index);
            }
        }

        return holding;
    }

    /*
    * The quadtree's selection covers the land once: every place on it lies in exactly one node; the nodes round the
    * eye are the finest level (64 m, 2 m apart at 32 quads), and coarser ones lie further out. Nodes wholly off the
    * land are left out.
    */
    void testTheSelectionCoversTheLandOnce() {
        const Terrain terrain = flatTerrain();
        const Vec3 eye{30.0f, 2.0f, -10.0f};
        const std::vector<TerrainNode> nodes = selectTerrainNodes(terrain, eye, ranges);
        assert(!nodes.empty());

        for (float x = -995.0f; x < 1000.0f; x += 37.0f) {
            for (float z = -995.0f; z < 1000.0f; z += 41.0f) {
                assert(nodesHolding(nodes, x, z).size() == 1);
            }
        }

        const std::vector<std::size_t> under = nodesHolding(nodes, eye.x, eye.z);
        assert(nodes[under.front()].level == 0 && nodes[under.front()].size == 64.0f && !nodes[under.front()].half);
        assert(nodes[nodesHolding(nodes, 900.0f, 900.0f).front()].level >= 3);

        for (const TerrainNode& node : nodes) {
            assert(node.corner.x < 1000.0f && node.corner.x + node.size > -1000.0f && node.corner.y < 1000.0f && node.corner.y + node.size > -1000.0f);
            assert(node.size == terrain.leafSize * static_cast<float>(1 << node.level) * (node.half ? 0.5f : 1.0f));
        }
    }

    /*
    * No cracks: where two nodes meet, their levels differ by one at most, and the finer one is fully morphed along the
    * edge (its points there at or past its morph's end), so its edge is the coarser one's grid.
    */
    void testNeighboursMeetWithoutCracks() {
        const Terrain terrain = flatTerrain();
        const Vec3 eye{30.0f, 2.0f, -10.0f};
        const std::vector<TerrainNode> nodes = selectTerrainNodes(terrain, eye, ranges);
        int levelChanges = 0;

        for (const TerrainNode& node : nodes) {
            // A point just past the middle of each side, into the neighbour.
            const float middle = node.size * 0.5f;
            const Vec2 sides[4] = {{node.corner.x - 0.5f, node.corner.y + middle}, {node.corner.x + node.size + 0.5f, node.corner.y + middle},
                                   {node.corner.x + middle, node.corner.y - 0.5f}, {node.corner.x + middle, node.corner.y + node.size + 0.5f}};
            const Vec2 edges[4] = {{node.corner.x, node.corner.y + middle}, {node.corner.x + node.size, node.corner.y + middle},
                                   {node.corner.x + middle, node.corner.y}, {node.corner.x + middle, node.corner.y + node.size}};

            for (int side = 0; side < 4; ++side) {
                const std::vector<std::size_t> other = nodesHolding(nodes, sides[side].x, sides[side].y);

                if (other.empty()) {
                    continue;
                }

                const TerrainNode& neighbour = nodes[other.front()];
                assert(std::abs(neighbour.level - node.level) <= 1);

                if (neighbour.level > node.level) {
                    ++levelChanges;
                    const Vec3 edge{edges[side].x, 0.0f, edges[side].y};
                    assert(terrainMorph((edge - eye).length(), node.morphStart, node.morphEnd) == 1.0f);
                }
            }
        }

        assert(levelChanges > 0);
    }

    /*
    * The morph's range for a level: ending at the level's range (firstRangeMetres doubled each level) and starting
    * morphShare of the way back to the finer level's range.
    */
    void testEachLevelMorphsAtTheEndOfItsRange() {
        assert(terrainRange(0, ranges) == 100.0f && terrainRange(3, ranges) == 800.0f);
        const TerrainNode node = terrainNodeAt(2, Vec2{0.0f, 0.0f}, 256.0f, false, ranges);
        assert(node.morphEnd == 400.0f && near(node.morphStart, 400.0f - 0.3f * 200.0f, 1e-4f) && node.spacing == 8.0f);
        const TerrainNode leaf = terrainNodeAt(0, Vec2{0.0f, 0.0f}, 64.0f, false, ranges);
        assert(leaf.morphEnd == 100.0f && near(leaf.morphStart, 70.0f, 1e-4f) && leaf.spacing == 2.0f);
        const TerrainNode quarter = terrainNodeAt(2, Vec2{0.0f, 0.0f}, 128.0f, true, ranges);
        assert(quarter.spacing == 8.0f);
    }

    /*
    * A patch's vertex as the shaders make it (terrain.glsl mirrors terrainVertex): its grid point placed on the node,
    * slid by the morph its distance from the eye gives, at the land's height there; the normal from the land's slope
    * over the level's spacing; the texture coordinates across its region's square (as the ground's rings had them); the
    * tangent along x in the surface.
    */
    void testAPatchVertexIsPlacedOnTheLand() {
        const std::shared_ptr<const TerrainHeights> heights = twoGrids(plane);
        TerrainPatch patch{.corner = Vec2{-16.0f, -16.0f}, .size = 64.0f, .quads = 32.0f, .spacing = 2.0f, .morphStart = 1000.0f, .morphEnd = 2000.0f,
                           .uvCorner = Vec2{-100.0f, -100.0f}, .uvPerMetre = 0.005f};
        const TerrainVertex vertex = terrainVertex(*heights, patch, Vec2{3.0f / 32.0f, 0.5f}, Vec3{0.0f, 0.0f, 0.0f});
        assert(near(vertex.position.x, -16.0f + 6.0f, 1e-4f) && near(vertex.position.z, 16.0f, 1e-4f));
        assert(near(vertex.position.y, plane(-10.0f, 16.0f), 1e-4f));
        assert(near(vertex.uv.x, (-10.0f + 100.0f) * 0.005f, 1e-5f) && near(vertex.uv.y, (16.0f + 100.0f) * 0.005f, 1e-5f));
        const Vec3 expected = Vec3{-0.5f, 1.0f, -0.25f}.normalized();
        assert(near(vertex.normal.x, expected.x, 1e-4f) && near(vertex.normal.y, expected.y, 1e-4f));
        const Vec3 along = Vec3{1.0f, 0.5f, 0.0f}.normalized();
        assert(near(vertex.tangent.x, along.x, 1e-4f) && near(vertex.tangent.y, along.y, 1e-4f) && near(vertex.tangent.z, 0.0f, 1e-5f) && vertex.tangentSign == -1.0f);

        // Fully morphed (far past the morph's end), an odd point lands on its even neighbour.
        patch.morphStart = 0.0f;
        patch.morphEnd = 1.0f;
        const TerrainVertex morphed = terrainVertex(*heights, patch, Vec2{3.0f / 32.0f, 0.5f}, Vec3{0.0f, 0.0f, 0.0f});
        assert(near(morphed.position.x, -16.0f + 4.0f, 1e-4f) && near(morphed.position.y, plane(-12.0f, 16.0f), 1e-4f));
    }

    /*
    * The terrain's draws: one a node, the full grid or the half grid, placed by its model matrix (the node's corner,
    * its size across x and z), its bounds round the node's box at the land's heights, still (last frame's matrix its
    * own), casting shadows, never static; each names its patch, and takes the material of the finest region holding it
    * whole.
    */
    void testTheTerrainBecomesDraws() {
        const Terrain terrain = flatTerrain();
        const Vec3 eye{30.0f, 2.0f, -10.0f};
        const std::vector<TerrainNode> nodes = selectTerrainNodes(terrain, eye, ranges);
        std::vector<DrawItem> draws(3);
        draws.reserve(3 + nodes.size());
        const DrawItem* before = draws.data();
        std::vector<TerrainPatch> patches;
        appendTerrainDraws(terrain, nodes, draws, patches);
        assert(draws.size() == 3 + nodes.size() && patches.size() == nodes.size());

        // Into the room the frame kept for them, the list never moving (a reserve of its own copied every draw).
        assert(draws.data() == before);

        bool sawRegions[2] = {false, false};

        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const DrawItem& draw = draws[3 + index];
            const TerrainNode& node = nodes[index];
            assert(draw.terrainPatch == static_cast<int>(index) && patches[index].corner.x == node.corner.x && patches[index].size == node.size);
            assert(draw.mesh == (node.half ? terrain.halfGrid : terrain.grid));
            assert(draw.model.values[0][3] == node.corner.x && draw.model.values[2][3] == node.corner.y && draw.model.values[0][0] == node.size);
            assert(draw.previousModel && *draw.previousModel == draw.model && draw.castsShadows && !draw.isStatic);
            assert(draw.bounds.radius >= node.size * 0.7f);

            const bool inner = node.corner.x >= -128.0f && node.corner.x + node.size <= 128.0f && node.corner.y >= -128.0f && node.corner.y + node.size <= 128.0f;
            assert(draw.material.roughness == (inner ? 0.25f : 0.75f));
            sawRegions[inner ? 0 : 1] = true;
            assert(patches[index].uvPerMetre == 1.0f / (2.0f * (inner ? 128.0f : 1024.0f)));
        }

        assert(sawRegions[0] && sawRegions[1]);

        // Rays never meet a patch: its mesh is a flat grid only the shaders lift onto the land.
        assert(rayMaskFor(draws[3]) == 0u);

        // The GPU's records: the instance names its patch (one past its index, 0 for none).
        assert(packInstance(draws[3], 0).material[2] == 1u && packInstance(draws[0], 0).material[2] == 0u);
        const GpuTerrainPatch packed = packTerrainPatch(patches[0]);
        assert(packed.placement[0] == patches[0].corner.x && packed.placement[2] == patches[0].size && packed.morph[1] == patches[0].morphEnd);
    }

    /*
    * The heights as one buffer: a header of each grid's corner, spacing, size and where its heights start, then every
    * grid's heights one after another.
    */
    void testTheHeightsArePackedForTheGpu() {
        const std::shared_ptr<const TerrainHeights> heights = twoGrids(plane);
        const std::vector<std::uint32_t> words = packTerrainHeights(*heights);
        GpuTerrainHeightsHeader header;
        std::memcpy(&header, words.data(), sizeof(header));
        assert(header.gridCount == 2);
        assert(header.grids[0].corner[0] == -32.0f && header.grids[0].spacing == 1.0f && header.grids[0].size == 65u && header.grids[0].offset == 0u);
        assert(header.grids[1].offset == 65u * 65u && header.grids[1].size == 65u);
        float second = 0.0f;
        std::memcpy(&second, &words[sizeof(header) / 4 + 1], sizeof(float));
        assert(second == heights->grids[0].heights[1]);
        assert(words.size() == sizeof(header) / 4 + 65u * 65u * 2u);
    }

    /*
    * A hidden draw is drawn by no view and met by no ray, but stays part of the static level (the sky visibility's
    * rays and the ground walked on): the forest's 2 m ground, now drawn as terrain patches.
    */
    void testHiddenDrawsAreDrawnByNoView() {
        std::vector<DrawItem> draws(2);
        draws[1].hidden = true;
        draws[0].bounds = BoundingSphere{Vec3{0.0f, 0.0f, -5.0f}, 1.0f};
        draws[1].bounds = draws[0].bounds;
        const Frustum all = Frustum::fromClipMatrix(Mat4::perspective(1.5f, 1.0f, 0.1f, 100.0f), ClipDepth::NegativeOneToOne);
        assert(visibleDraws(draws, all).size() == 1);
        assert(castersInReach(draws, BoundingSphere{}).size() == 1);
        assert(rayMaskFor(draws[1]) == 0u);
    }

    /*
    * A box's range of heights holds every height the land gives inside it, drawn or bilinear: boxes inside the fine grid,
    * across both grids' edges and past every grid (where the coarsest's nearest edge stands). Within the fine grid it is
    * near the land's own range there, not the whole grid's.
    */
    void testABoxsRangeHoldsItsHeights() {
        const std::shared_ptr<const TerrainHeights> wavy = twoGrids(waves);
        std::uint32_t state = 7;
        const auto unit = [&state] {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / 16777216.0f;
        };

        for (int test = 0; test < 300; ++test) {
            const float reach = test < 100 ? 30.0f : (test < 200 ? 600.0f : 3000.0f);
            const float west = (unit() - 0.5f) * 2.0f * reach;
            const float north = (unit() - 0.5f) * 2.0f * reach;
            const float east = west + 1.0f + unit() * 64.0f;
            const float south = north + 1.0f + unit() * 64.0f;
            const TerrainHeightRange range = terrainHeightRange(*wavy, west, east, north, south);
            assert(range.lowest <= range.highest);

            for (int sample = 0; sample < 40; ++sample) {
                const float x = west + unit() * (east - west);
                const float z = north + unit() * (south - north);
                const float drawn = terrainSurfaceHeight(*wavy, x, z);
                const float bilinear = terrainHeight(*wavy, x, z);
                assert(drawn >= range.lowest && drawn <= range.highest);
                assert(bilinear >= range.lowest && bilinear <= range.highest);
            }
        }

        // A small box in the fine grid: no wider than its own squares' heights (waves change at most 0.2 m a metre).
        const TerrainHeightRange small = terrainHeightRange(*wavy, 3.2f, 5.7f, -4.1f, -2.2f);
        assert(small.highest - small.lowest < 1.5f);
        assert(terrainHeightRange(TerrainHeights{}, 0.0f, 1.0f, 0.0f, 1.0f).lowest == 0.0f);
    }
}

void testTerrain() {
    testHeightsAreSampledFromTheFinestGrid();
    testTheDrawnSurfaceIsTheGridsTriangles();
    testTheMorphSlidesOddPointsOntoEvenOnes();
    testTheSelectionCoversTheLandOnce();
    testNeighboursMeetWithoutCracks();
    testEachLevelMorphsAtTheEndOfItsRange();
    testAPatchVertexIsPlacedOnTheLand();
    testTheTerrainBecomesDraws();
    testTheHeightsArePackedForTheGpu();
    testHiddenDrawsAreDrawnByNoView();
    testABoxsRangeHoldsItsHeights();
}
