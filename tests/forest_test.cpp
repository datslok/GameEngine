#include "app/game_registry.h"
#include "core/command_registry.h"
#include "games/forest/forest_game.h"
#include "games/forest/forest_ground.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/forest_trees.h"
#include "games/forest/valley_shapes.h"
#include "games/forest/valley_strata.h"
#include "physics/static_level.h"
#include "render/gpu/ground_layers.h"
#include "scene/atmosphere.h"
#include "scene/fog.h"
#include "scene/light.h"
#include "scene/lighting.h"
#include "math/transform.h"
#include "scene/model_renderer.h"
#include "scene/indexed_mesh.h"
#include "scene/still_draws.h"
#include "scene/terrain.h"

#include "forest_helpers.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::fabs(a - b) <= tolerance;
    }

    // The steepness of the ground at a point: 0 level, 1 a cliff (one less the up component of its normal).
    float slopeAt(float x, float z) {
        return 1.0f - forestNormal(x, z).y;
    }

    // The layer painted most at a point.
    std::size_t strongestLayer(const GroundWeights& weights) {
        std::size_t strongest = 0;

        for (std::size_t layer = 1; layer < weights.size(); ++layer) {
            if (weights[layer] > weights[strongest]) {
                strongest = layer;
            }
        }

        return strongest;
    }

    Vec3 centroid(const Mesh& mesh, const Triangle& triangle) {
        const Vec4& a = mesh.vertices[triangle.first];
        const Vec4& b = mesh.vertices[triangle.second];
        const Vec4& c = mesh.vertices[triangle.third];
        return Vec3{(a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f};
    }

    // The mean of one row of a wrapped surface's points: the middle of that ring of a pillar or an island.
    Vec3 rowCentre(const Mesh& mesh, std::size_t row, std::size_t columns) {
        Vec3 sum{0.0f, 0.0f, 0.0f};

        for (std::size_t column = 0; column < columns; ++column) {
            const Vec4& point = mesh.vertices[row * columns + column];
            sum = sum + Vec3{point.x, point.y, point.z};
        }

        return sum * (1.0f / static_cast<float>(columns));
    }

    /*
    * A surface of rows faces outwards: every triangle facing sideways faces away from the middle of its first corner's
    * row, and the volume it closes (the sum of the tetrahedra its triangles make with the first row's middle, which an
    * open first row in one plane leaves out) is positive. Its normals are unit long.
    */
    void checkFacesOutFromRows(const Mesh& mesh, std::size_t columns) {
        assert(!mesh.triangles.empty());
        const Vec3 origin = rowCentre(mesh, 0, columns);
        float volume = 0.0f;

        for (const Triangle& triangle : mesh.triangles) {
            const Vec4& a = mesh.vertices[triangle.first];
            const Vec4& b = mesh.vertices[triangle.second];
            const Vec4& c = mesh.vertices[triangle.third];
            const Vec3 normal = faceNormal(a, b, c);
            const Vec3 outward = centroid(mesh, triangle) - rowCentre(mesh, triangle.first / columns, columns);

            if (std::fabs(normal.y) < 0.5f) {
                const Vec3 sideways{outward.x, 0.0f, outward.z};
                assert(normal.x * sideways.x + normal.z * sideways.z > 0.0f);
            }

            const Vec3 first = Vec3{a.x, a.y, a.z} - origin;
            const Vec3 second = Vec3{b.x, b.y, b.z} - origin;
            const Vec3 third = Vec3{c.x, c.y, c.z} - origin;
            volume += first.dot(second.cross(third)) / 6.0f;

            for (std::size_t corner = 0; corner < 3; ++corner) {
                assert(triangle.normals[corner].has_value() && near(triangle.normals[corner]->length(), 1.0f, 1e-3f));
                assert(triangle.uvs[corner].has_value());
            }
        }

        assert(volume > 0.0f);
    }

    // A fresh valley on the shared job system (forest_helpers.h).
    using Forest = TestForest;

    // It is a registered game, started with --game forest, and its title names its settings section.
    void testItIsRegistered() {
        const GameEntry* entry = findGame("forest");
        assert(entry != nullptr);
        assert(parseLaunchOptions({"--game", "forest"}).game == entry);

        Forest forest;
        assert(std::string{forest.game.title()} == "Forest");
    }

    // The noise is the same every time for the same place and seed, stays in its range, and changes smoothly.
    void testNoiseIsSmoothAndRepeatable() {
        float lowest = 1.0f;
        float highest = 0.0f;

        for (int step = 0; step < 2000; ++step) {
            const float x = -97.3f + 0.37f * static_cast<float>(step);
            const float z = 41.9f - 0.21f * static_cast<float>(step);
            const float value = forestValueNoise(x, z, 7u);
            assert(value == forestValueNoise(x, z, 7u));
            assert(value >= 0.0f && value <= 1.0f);
            assert(std::fabs(forestValueNoise(x + 0.01f, z, 7u) - value) < 0.05f);
            lowest = std::min(lowest, value);
            highest = std::max(highest, value);

            const float fractal = forestFractalNoise(x, z, 4, 3u);
            assert(fractal >= -1.0f && fractal <= 1.0f);
        }

        assert(highest - lowest > 0.6f);
        assert(forestValueNoise(10.5f, 3.25f, 1u) != forestValueNoise(10.5f, 3.25f, 2u));

        // No square grain: the fractal noise's slopes point every way alike, as few within 10 degrees of an axis as
        // random directions give (2/9), where the unturned octaves gave a third.
        int alongAxis = 0;
        int slopes = 0;

        for (int row = 0; row < 300; ++row) {
            for (int column = 0; column < 300; ++column) {
                const float x = 0.137f * static_cast<float>(column) + 3.1f;
                const float z = 0.113f * static_cast<float>(row) - 7.7f;
                const float step = 1e-3f;
                const float slopeX = forestFractalNoise(x + step, z, 3, 11u) - forestFractalNoise(x - step, z, 3, 11u);
                const float slopeZ = forestFractalNoise(x, z + step, 3, 11u) - forestFractalNoise(x, z - step, 3, 11u);

                if (slopeX * slopeX + slopeZ * slopeZ < 1e-12f) {
                    continue;
                }

                const float degrees = std::fmod(std::atan2(slopeZ, slopeX) * 57.29578f + 360.0f, 90.0f);
                alongAxis += degrees < 10.0f || degrees > 80.0f ? 1 : 0;
                ++slopes;
            }
        }

        assert(static_cast<float>(alongAxis) / static_cast<float>(slopes) < 0.25f);
    }

    /*
    * The valley: its floor near 0 with only the small hills on it, walls climbing to the rim about 150 m up, the ramp
    * climbing to the rim where the walk starts, mountains beyond, straight along the walk and winding further north,
    * and a level clearing at the Monolith's foot.
    */
    void testTheValleyHasAFloorWallsAndARim() {
        for (const float z : {-100.0f, -1000.0f, -3000.0f}) {
            const float axis = valleyAxis(z);
            assert(std::fabs(forestHeight(axis + 60.0f, z)) < 25.0f);
            assert(forestHeight(axis + 450.0f, z) > valleyRimHeight - 30.0f);
            assert(forestHeight(axis - 450.0f, z) > valleyRimHeight - 30.0f);
        }

        assert(forestHeight(forestStartPlace.x, forestStartPlace.y) > valleyRimHeight - 30.0f);
        assert(forestHeight(3500.0f, -2000.0f) > valleyRimHeight + 300.0f);
        assert(valleyAxis(0.0f) == 0.0f && valleyAxis(-600.0f) == 0.0f);

        float widest = 0.0f;

        for (float z = -2000.0f; z > -12000.0f; z -= 100.0f) {
            widest = std::max(widest, std::fabs(valleyAxis(z)));
        }

        assert(widest > 300.0f && widest <= 400.0f);

        const float level = forestHeight(forestClearingCentre.x, forestClearingCentre.y);

        for (float angle = 0.0f; angle < 6.28f; angle += 0.5f) {
            const float x = forestClearingCentre.x + 0.8f * forestClearingRadius * std::cos(angle);
            const float z = forestClearingCentre.y + 0.8f * forestClearingRadius * std::sin(angle);

            if (forestPathDistance(x, z) < forestPathHalfWidth + forestPathBlend) {
                continue;
            }

            assert(near(forestHeight(x, z), level, 0.3f));
            assert(slopeAt(x, z) < 0.02f);
        }
    }

    // The path: from the start down the ramp, through the Ring's middle, into the clearing; level across, walkable along.
    void testThePathRunsFromTheRimThroughTheRingToTheClearing() {
        const std::vector<Vec2>& path = forestPathPoints();
        assert(path.size() >= 5);
        assert((path.front() - forestStartPlace).length() < 1e-3f);
        assert((path.back() - forestClearingCentre).length() < forestClearingRadius);
        assert(forestPathDistance(forestRingPlace.x, forestRingPlace.y) < 0.01f);

        for (std::size_t point = 0; point + 1 < path.size(); ++point) {
            const Vec2 start = path[point];
            const Vec2 along = path[point + 1] - start;
            const Vec2 across = Vec2{-along.y, along.x} * (1.0f / along.length());

            for (float share = 0.0f; share < 1.0f; share += 0.05f) {
                const Vec2 centre = start + along * share;
                assert(forestPathDistance(centre.x, centre.y) < 0.01f);
                // Level across, away from the bends (where the inside reads the path's level a little further along it).
                const Vec2 side = centre + across * (0.6f * forestPathHalfWidth);

                if (share > 0.08f && share < 0.92f) {
                    assert(near(forestHeight(side.x, side.y), forestHeight(centre.x, centre.y), 0.02f));
                }

                const Vec2 ahead = centre + along * (0.5f / along.length());
                const float rise = std::fabs(forestHeight(ahead.x, ahead.y) - forestHeight(centre.x, centre.y)) / 0.5f;
                assert(rise < 0.5f); // about 27 degrees: a mountain path, well under the walker's 45
            }
        }
    }

    /*
    * The rings: four, each round the last, reaching 20 km; a spacing that would split a ring's squares is refused. Each
    * ring's squares face up, none in its hole, and its outer border lies exactly on the next ring's coarser edge, so the
    * two meet with no crack.
    */
    void testTheRingsMeetWithoutCracks() {
        const std::vector<TerrainRing> rings = forestTerrainRings(8.0f);
        assert(rings.size() == 4 && rings.front().innerHalfSize == 0.0f && rings.back().outerHalfSize >= 20000.0f);
        assert(forestTerrainRings(2.0f).front().spacing == 2.0f);

        bool refused = false;

        try {
            forestTerrainRings(3.0f);
        } catch (const std::invalid_argument&) {
            refused = true;
        }

        assert(refused);

        for (std::size_t index = 0; index < rings.size(); ++index) {
            const TerrainRing& ring = rings[index];

            if (index + 1 < rings.size()) {
                assert(rings[index + 1].innerHalfSize == ring.outerHalfSize && rings[index + 1].spacing == ring.outerSpacing);
                assert(ring.outerSpacing > ring.spacing);
            }

            const Mesh mesh = forestTerrainRingMesh(ring, forestFarExtent());
            const int squares = static_cast<int>(2.0f * ring.outerHalfSize / ring.spacing);
            const int hole = static_cast<int>(2.0f * ring.innerHalfSize / ring.spacing);
            assert(mesh.triangles.size() == static_cast<std::size_t>(2 * (squares * squares - hole * hole)));

            for (const Triangle& triangle : mesh.triangles) {
                assert(faceNormal(mesh.vertices[triangle.first], mesh.vertices[triangle.second], mesh.vertices[triangle.third]).y > 0.0f);
                const Vec3 middle = centroid(mesh, triangle);
                const bool inHole = std::fabs(middle.x - forestTerrainCentre.x) < ring.innerHalfSize &&
                                    std::fabs(middle.z - forestTerrainCentre.y) < ring.innerHalfSize;
                assert(!inHole);
            }

            // The outer border: on the straight line between the next ring's points either side.
            for (const Vec4& point : mesh.vertices) {
                const float localX = point.x - forestTerrainCentre.x;
                const float localZ = point.z - forestTerrainCentre.y;

                if (index + 1 == rings.size() || !near(std::fabs(localZ), ring.outerHalfSize, 1e-3f)) {
                    continue;
                }

                const float steps = (localX + ring.outerHalfSize) / ring.outerSpacing;
                const float lower = std::floor(steps);
                const float firstX = point.x - (steps - lower) * ring.outerSpacing;
                const float first = forestHeight(firstX, point.z);
                const float second = forestHeight(firstX + ring.outerSpacing, point.z);
                assert(near(point.y, first + (second - first) * (steps - lower), 1e-2f));
            }
        }

        // Inside the border a ring follows the land.
        const TerrainRing& nearRing = rings.front();
        assert(forestRingHeight(10.0f, 20.0f, nearRing) == forestHeight(10.0f, 20.0f));
    }

    // The valley's carving as the game makes it, with a smaller warp texture for speed.
    RockCarving valleyCarving() {
        return RockCarving{.bands = valleyStrata(), .warp = strataWarpImage(64), .warpTileMetres = 1500.0f, .warpMetres = 6.0f};
    }

    /*
    * The beds' relief: a hard bed stands out its ledge, the soft rock right under one is cut back the most, less further
    * down, nothing past the reach, and soft rock under soft rock is left alone.
    */
    void testHardBedsStandOut() {
        const std::vector<StrataBand> bands{{.bottom = 0.0f, .top = 30.0f}, {.bottom = 30.0f, .top = 40.0f, .hard = true}, {.bottom = 40.0f, .top = 90.0f},
                                            {.bottom = 90.0f, .top = 120.0f, .tone = 0.5f}};
        assert(strataRelief(bands, 35.0f) == rockLedgeMetres);
        assert(near(strataRelief(bands, 29.99f), -rockUndercutMetres, 0.01f));
        assert(near(strataRelief(bands, 28.0f), -rockUndercutMetres * 0.5f, 1e-4f));
        assert(strataRelief(bands, 20.0f) == 0.0f);
        assert(strataRelief(bands, 89.0f) == 0.0f && strataRelief(bands, 100.0f) == 0.0f);
        assert(strataRelief(bands, 500.0f) == 0.0f && strataRelief(bands, -50.0f) == 0.0f);
    }

    /*
    * The joints: shares from 0 to 1, a few narrow cracks round a pillar (more round a wider one), each running up it: a
    * crack at one height is still there a row higher.
    */
    void testJointsAreNarrowVerticalCracks() {
        const auto cracks = [](float radius, float height) {
            int count = 0;
            bool inside = false;

            for (int step = 0; step < 720; ++step) {
                const float groove = jointGroove(6.2831853f * static_cast<float>(step) / 720.0f, height, radius, 5u);
                assert(groove >= 0.0f && groove <= 1.0f);
                count += !inside && groove > 0.5f ? 1 : 0;
                inside = groove > 0.5f;
            }

            return count;
        };

        const int narrow = cracks(15.0f, 100.0f);
        const int wide = cracks(45.0f, 100.0f);
        assert(narrow >= 2 && wide > narrow && wide < 60);

        int kept = 0;
        int deep = 0;

        for (int step = 0; step < 720; ++step) {
            const float angle = 6.2831853f * static_cast<float>(step) / 720.0f;

            if (jointGroove(angle, 100.0f, 30.0f, 5u) > 0.8f) {
                deep += 1;
                kept += jointGroove(angle, 100.0f + rockRowMetres, 30.0f, 5u) > 0.4f ? 1 : 0;
            }
        }

        assert(deep > 0 && kept * 10 >= deep * 8);
    }

    /*
    * The bed waves on the shapes match the shader's: the warp texture's texel at its centre gives that texel's move, it
    * repeats every tile, and nothing moves without an amplitude.
    */
    void testShapesWaveTheirBedsAsTheShaderDoes() {
        const RockCarving carving = valleyCarving();
        const float texelMetres = carving.warpTileMetres / static_cast<float>(carving.warp.width);
        const float x = 10.5f * texelMetres;
        const float z = 3.5f * texelMetres;
        const float texel = static_cast<float>(carving.warp.pixels[(3 * carving.warp.width + 10) * 4]) / 255.0f;
        assert(near(carvingWarp(carving, x, z), (2.0f * texel - 1.0f) * carving.warpMetres, 1e-4f));
        assert(near(carvingWarp(carving, x + carving.warpTileMetres, z - 2.0f * carving.warpTileMetres), carvingWarp(carving, x, z), 1e-3f));

        RockCarving level = carving;
        level.warpMetres = 0.0f;
        assert(carvingWarp(level, x, z) == 0.0f);
    }

    /*
    * The ledges are where the beds are: the same pillar carved by beds all hard stands out from the one carved by beds
    * all soft by the ledge, everywhere but its buried foot and top, so the relief follows the beds, not the shape.
    */
    void testPillarsStandOutAtHardBeds() {
        RockCarving soft{.bands = {{.bottom = -1000.0f, .top = 2000.0f}}, .warp = {}, .warpTileMetres = 1500.0f, .warpMetres = 0.0f};
        RockCarving hard = soft;
        hard.bands.front().hard = true;
        const PillarPlacement placement{.foot = Vec3{0.0f, 0.0f, 0.0f}, .radius = 20.0f, .height = 220.0f, .turnRadians = 0.0f, .seed = 9u};
        const Mesh softPillar = wrappedSurfaceMesh(pillarSurface(placement, soft));
        const Mesh hardPillar = wrappedSurfaceMesh(pillarSurface(placement, hard));
        assert(softPillar.vertices.size() == hardPillar.vertices.size());
        float sumOut = 0.0f;
        int counted = 0;

        for (std::size_t vertex = 0; vertex < softPillar.vertices.size(); ++vertex) {
            const Vec4& a = softPillar.vertices[vertex];
            const Vec4& b = hardPillar.vertices[vertex];

            if (a.y > 10.0f && a.y < 200.0f) {
                sumOut += Vec2{b.x, b.z}.length() - Vec2{a.x, a.z}.length();
                counted += 1;
            }
        }

        assert(counted > 1000 && near(sumOut / static_cast<float>(counted), rockLedgeMetres, 0.4f));
    }

    /*
    * The render format straight from a surface's points: one vertex a point at its place, the same triangles and
    * normals as the Mesh built from the same points, and each tangent square to its normal, running round the surface
    * (up the picture, w times normal cross tangent, is up the surface).
    */
    void testSurfacesGoStraightToTheRenderFormat() {
        const PillarPlacement placement{.foot = Vec3{0.0f, 0.0f, 0.0f}, .radius = 14.0f, .height = 210.0f, .turnRadians = 0.0f, .seed = 6u};
        const SurfaceGrid grid = pillarSurface(placement, valleyCarving());
        const Mesh mesh = wrappedSurfaceMesh(grid);
        const IndexedMesh indexed = wrappedSurfaceIndexedMesh(grid);
        assert(indexed.vertices.size() == grid.points.size() && indexed.indices.size() == 3 * mesh.triangles.size());

        for (std::size_t triangle = 0; triangle < mesh.triangles.size(); ++triangle) {
            const Triangle& expected = mesh.triangles[triangle];
            assert(indexed.indices[3 * triangle] == expected.first && indexed.indices[3 * triangle + 1] == expected.second &&
                   indexed.indices[3 * triangle + 2] == expected.third);
            const Vec3& normal = indexed.vertices[expected.first].normal;
            assert(near((normal - *expected.normals[0]).length(), 0.0f, 1e-6f));
        }

        for (std::size_t point = grid.columns; point + grid.columns < grid.points.size(); point += 37) {
            const MeshVertex& vertex = indexed.vertices[point];
            const Vec3 tangent{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z};
            assert(vertex.position.x == grid.points[point].x && near(tangent.length(), 1.0f, 1e-4f) && near(tangent.dot(vertex.normal), 0.0f, 1e-4f));
            const Vec3 up = vertex.normal.cross(tangent) * vertex.tangent.w;
            assert(std::fabs(vertex.normal.y) > 0.6f || up.y > 0.0f);
        }
    }

    // A wrapped surface needs enough rows and columns; a pillar, an island and the Ring face outwards with smooth normals.
    void testTheShapesFaceOutwards() {
        bool refused = false;

        try {
            wrappedSurfaceMesh(1, 2, false, [](int, int) { return Vec3{0.0f, 0.0f, 0.0f}; });
        } catch (const std::invalid_argument&) {
            refused = true;
        }

        assert(refused);

        const RockCarving carving = valleyCarving();
        const PillarPlacement placement{.foot = Vec3{120.0f, 4.0f, -900.0f}, .radius = 25.0f, .height = 280.0f, .turnRadians = 0.6f, .seed = 3u};
        const Mesh pillar = wrappedSurfaceMesh(pillarSurface(placement, carving));
        const std::size_t columns = static_cast<std::size_t>(std::lround(6.2831853f * 25.0f / rockColumnMetres));
        checkFacesOutFromRows(pillar, columns);
        float lowest = 1e9f;
        float highest = -1e9f;

        for (const Vec4& point : pillar.vertices) {
            lowest = std::min(lowest, point.y);
            highest = std::max(highest, point.y);
        }

        // Its own size in metres: buried a twentieth of its height, its broken top a little over its height.
        assert(near(lowest, -pillarBuriedDepth * 280.0f, 1e-3f) && highest > 280.0f && highest < 284.0f);
        assert(pillar.vertices.size() > 100 * columns);
        assert(wrappedSurfaceMesh(pillarSurface(placement, carving)).vertices[100].x == pillar.vertices[100].x);
        PillarPlacement other = placement;
        other.seed = 4u;
        assert(wrappedSurfaceMesh(pillarSurface(other, carving)).vertices[100].x != pillar.vertices[100].x);

        const IslandPlacement islandPlace{.rim = Vec3{-300.0f, 620.0f, -2000.0f}, .radius = 60.0f, .turnRadians = 1.0f, .seed = 2u};
        const Mesh island = wrappedSurfaceMesh(islandSurface(islandPlace, carving));
        const std::size_t islandColumns = static_cast<std::size_t>(std::lround(6.2831853f * 60.0f / (1.2f * rockColumnMetres)));
        checkFacesOutFromRows(island, islandColumns);
        lowest = 0.0f;
        highest = -1e9f;

        for (const Vec4& point : island.vertices) {
            lowest = std::min(lowest, point.y);
            highest = std::max(highest, point.y);
        }

        assert(near(lowest, -islandDepth * 60.0f, 1e-3f) && highest > 0.0f && highest < 0.15f * 60.0f);

    }

    /*
    * The pillars and islands: as many as asked, the same every run, spaced apart, at their sizes, clear of the path,
    * the Ring, the clearing and the start; the islands above the pillars under them. The Monolith's pillar is
    * tall enough for its strut (forest_structures_test has the rest).
    */
    void testShapesArePlacedApartAndClearOfTheWalk() {
        const ValleyLayout layout{};
        const std::vector<PillarPlacement> pillars = placePillars(layout);
        assert(static_cast<int>(pillars.size()) == layout.pillarCount);
        assert(placePillars(layout).back().foot.x == pillars.back().foot.x);

        for (std::size_t index = 0; index < pillars.size(); ++index) {
            const PillarPlacement& pillar = pillars[index];
            assert(pillar.height >= 200.0f && pillar.height <= 350.0f && pillar.radius >= 12.0f && pillar.radius <= 45.0f);
            assert(forestPathDistance(pillar.foot.x, pillar.foot.z) > 40.0f);
            assert((Vec2{pillar.foot.x, pillar.foot.z} - forestStartPlace).length() > 250.0f);
            assert((Vec2{pillar.foot.x, pillar.foot.z} - forestClearingCentre).length() > forestClearingRadius);
            assert(pillar.foot.y <= forestHeight(pillar.foot.x, pillar.foot.z) + 1e-3f);
            assert(index == 0 || pillar.seed != pillars[index - 1].seed);

            for (std::size_t other = index + 1; other < pillars.size(); ++other) {
                const float apart = (Vec2{pillar.foot.x, pillar.foot.z} - Vec2{pillars[other].foot.x, pillars[other].foot.z}).length();
                assert(apart >= pillar.radius + pillars[other].radius + 70.0f);
            }
        }

        const std::vector<IslandPlacement> islands = placeIslands(layout, pillars);
        assert(static_cast<int>(islands.size()) == layout.islandCount);

        for (std::size_t index = 0; index < islands.size(); ++index) {
            const IslandPlacement& island = islands[index];
            const float tip = island.rim.y - islandDepth * island.radius;
            assert(island.radius >= 25.0f && island.radius <= 150.0f);
            assert(tip >= std::max(forestHeight(island.rim.x, island.rim.z), 0.0f) + 380.0f - 1e-2f);
            assert((Vec2{island.rim.x, island.rim.z} - forestStartPlace).length() > 400.0f);

            for (const PillarPlacement& pillar : pillars) {
                if ((Vec2{island.rim.x, island.rim.z} - Vec2{pillar.foot.x, pillar.foot.z}).length() < island.radius + pillar.radius) {
                    assert(pillar.foot.y + pillar.height < tip);
                }
            }

            for (std::size_t other = index + 1; other < islands.size(); ++other) {
                const float apart = (Vec2{island.rim.x, island.rim.z} - Vec2{islands[other].rim.x, islands[other].rim.z}).length();
                assert(apart >= island.radius + islands[other].radius + 120.0f);
            }
        }

        const float level = 3.0f;
        const PillarPlacement bridgePillar = monolithPillar(level);
        assert(bridgePillar.foot.y + bridgePillar.height > level + 340.0f);
    }

    /*
    * The ground's paint: every place's weights add up to one; grass in the clearing and the start's meadow, mud on the
    * path, mossy rock on the valley's walls, leaf litter under the forest on the floor elsewhere.
    */
    void testTheGroundIsPaintedByPlace() {
        for (float x = -600.0f; x <= 600.0f; x += 37.0f) {
            for (float z = -400.0f; z <= 800.0f; z += 37.0f) {
                const GroundWeights paint = forestGroundPaint(x, z);
                assert(near(paint[0] + paint[1] + paint[2] + paint[3], 1.0f, 1e-4f));

                for (const float weight : paint) {
                    assert(weight >= 0.0f && weight <= 1.0f);
                }
            }
        }

        assert(strongestLayer(forestGroundPaint(forestClearingCentre.x + 40.0f, forestClearingCentre.y + 30.0f)) == forestGrassLayer);
        assert(forestMeadowShare(forestStartPlace.x, forestStartPlace.y) > 0.9f);
        assert(strongestLayer(forestGroundPaint(forestStartPlace.x + 20.0f, forestStartPlace.y + 15.0f)) == forestGrassLayer);

        const std::vector<Vec2>& path = forestPathPoints();
        const Vec2 onPath = (path[2] + path[3]) * 0.5f;
        assert(strongestLayer(forestGroundPaint(onPath.x, onPath.y)) == forestMudLayer);

        bool foundSteep = false;
        bool foundForest = false;

        for (float x = -400.0f; x <= 400.0f; x += 4.0f) {
            for (float z = -250.0f; z <= 50.0f; z += 6.0f) {
                if (forestPathDistance(x, z) < 6.0f) {
                    continue;
                }

                if (slopeAt(x, z) > 0.25f) {
                    foundSteep = true;
                    assert(strongestLayer(forestGroundPaint(x, z)) == forestRockLayer);
                }

                if (slopeAt(x, z) < 0.05f && forestMeadowShare(x, z) < 0.01f) {
                    foundForest = true;
                    assert(strongestLayer(forestGroundPaint(x, z)) == forestLitterLayer);
                }
            }
        }

        assert(foundSteep && foundForest);
    }

    /*
    * The blend depth for a ring's paint: the setting's own on the finest texels, deeper as they grow (by the square root
    * of how many times coarser), never past 1 (a plain cross-fade), and never shallower than the setting.
    */
    void testCoarsePaintBlendsSofter() {
        assert(near(forestBlendDepth(0.2f, forestFinestPaintTexel), 0.2f, 1e-6f));
        assert(near(forestBlendDepth(0.2f, 4.0f * forestFinestPaintTexel), 0.4f, 1e-6f));
        assert(forestBlendDepth(0.2f, 64.0f * forestFinestPaintTexel) == 1.0f);
        assert(forestBlendDepth(0.2f, 0.5f * forestFinestPaintTexel) == 0.2f);
        assert(forestBlendDepth(0.0f, 100.0f) == 0.0f);
    }

    // The weights texture: the paint at each texel's centre across its extent, one channel a layer, as linear bytes.
    void testTheWeightsTextureHoldsThePaint() {
        const int size = 32;
        const GroundExtent extent = forestNearExtent();
        const ImageData image = forestWeightsImage(size, extent);
        assert(image.width == static_cast<std::uint32_t>(size) && image.height == static_cast<std::uint32_t>(size));
        assert(image.encoding == TextureEncoding::Linear && image.pixels.size() == static_cast<std::size_t>(size * size * 4));
        assert(image.storage == TextureStorage::Exact); // blend weights, never block-compressed
        assert(near(forestTextureU(extent.centre.x - extent.halfSize, extent), 0.0f, 1e-6f));
        assert(near(forestTextureV(extent.centre.y + extent.halfSize, extent), 1.0f, 1e-6f));
        const float texelMetres = 2.0f * extent.halfSize / static_cast<float>(size);

        for (int row = 0; row < size; row += 5) {
            for (int column = 0; column < size; column += 3) {
                const float x = forestTexelCentreX(column, size, extent);
                const float z = forestTexelCentreZ(row, size, extent);
                assert(near(forestTextureU(x, extent), (static_cast<float>(column) + 0.5f) / static_cast<float>(size), 1e-5f));
                const GroundWeights paint = forestGroundPaint(x, z, texelMetres);

                for (std::size_t layer = 0; layer < 4; ++layer) {
                    const std::uint8_t byte = image.pixels[(static_cast<std::size_t>(row) * size + column) * 4 + layer];
                    assert(std::abs(static_cast<int>(byte) - static_cast<int>(std::lround(paint[layer] * 255.0f))) <= 1);
                }
            }
        }
    }

    /*
    * A layer's textures are found by their roles' file names in its folder (the fetch tool's): Poly Haven's packed
    * occlusion-roughness-metal map or ambientCG's separate roughness and occlusion. None found: a plain stand-in colour.
    */
    void testLayerFilesAreFoundByRole() {
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "game_engine_forest_layer";
        std::filesystem::remove_all(folder);
        std::filesystem::create_directories(folder);

        for (const char* name : {"colour.jpg", "normal.png", "arm.jpg", "height.png"}) {
            std::ofstream{folder / name} << "x";
        }

        const ForestTextureFiles packed = findForestTextureFiles(folder.string());
        assert(packed.colour == (folder / "colour.jpg").string() && packed.normal == (folder / "normal.png").string());
        assert(packed.packedSurface == (folder / "arm.jpg").string() && packed.height == (folder / "height.png").string());
        assert(packed.roughness.empty() && packed.occlusion.empty());

        std::filesystem::remove(folder / "arm.jpg");
        std::ofstream{folder / "roughness.png"} << "x";
        std::ofstream{folder / "occlusion.png"} << "x";
        const ForestTextureFiles separate = findForestTextureFiles(folder.string());
        assert(separate.packedSurface.empty() && separate.roughness == (folder / "roughness.png").string());
        assert(separate.occlusion == (folder / "occlusion.png").string());
        std::filesystem::remove_all(folder);

        assert(findForestTextureFiles("no-such-folder/Grass004").colour.empty());

        AssetManager assets;
        const Material standIn = forestLayerMaterial(assets, "no-such-folder", forestGroundLayers[forestGrassLayer]);
        assert(!standIn.texture.isValid() && !standIn.normalTexture.isValid() && !standIn.heightTexture.isValid());
        assert(standIn.colour.g == forestGroundLayers[forestGrassLayer].standIn.g && assets.getTextureCount() == 0);
    }

    /*
    * The game: the finest ring of ground static and walked on, the coarser rings only drawn; the pillars and islands
    * asked for (plus the bridge's pillar); the Ring across the path; the Monolith static and solid; the sun through a
    * real sky, the haze; a far plane past the land; the player standing where they start and staying on the ground.
    */
    void testTheGameBuildsTheValley() {
        Forest forest;
        const ModelRenderer& renderer = forest.world.get<ModelRenderer>(forest.game.getGround());
        assert(renderer.isStatic && renderer.parts.size() == 1);
        const Material& material = renderer.parts.front().material;
        assert(material.groundLayers.size() == 4 && material.groundWeights.isValid());
        assert(near(material.groundRepeats[0], 2.0f * forestNearHalfSize / forestGroundLayers[0].tileMetres, 1e-2f));

        /*
        * The ground walked on is hidden (drawn by no view, casting no shadow, but still in the static level the sky
        * visibility's rays see); the land is drawn as terrain patches (scene/terrain.h): a region a ring, the finest the
        * ground's own material, each coarser one its own weights texture, blended softer; heights in three grids, the
        * finest at the ground's spacing, reading the land's heights at their points.
        */
        assert(renderer.hidden && !renderer.castsShadows);
        const Terrain& terrain = forest.world.get<Terrain>(forest.game.getTerrain());
        assert(terrain.regions.size() == 4 && terrain.regions.front().material.groundWeights == material.groundWeights);
        assert(terrain.grid.isValid() && terrain.halfGrid.isValid() && terrain.heights && terrain.heights->grids.size() == 3 && terrain.bounds);
        assert(terrain.landHalfSize == forestTerrainRings(8.0f).back().outerHalfSize);
        assert(terrain.leafSize == 8.0f * TERRAIN_GRID_QUADS && terrain.leafSize * static_cast<float>(1 << (terrain.levels - 1)) >= 2.0f * terrain.landHalfSize);
        const TerrainHeightGrid& finest = terrain.heights->grids.front();
        assert(finest.spacing == 8.0f && near(finest.heights[5 * finest.size + 7], forestHeight(finest.corner.x + 56.0f, finest.corner.y + 40.0f), 1e-4f));
        TextureHandle lastWeights = material.groundWeights;
        float lastDepth = material.groundBlendDepth;

        for (std::size_t region = 1; region < terrain.regions.size(); ++region) {
            const Material& farMaterial = terrain.regions[region].material;
            assert(farMaterial.groundWeights.isValid() && farMaterial.groundWeights != lastWeights);
            assert(farMaterial.groundBlendDepth >= lastDepth);
            assert(terrain.regions[region].halfSize > terrain.regions[region - 1].halfSize);
            lastWeights = farMaterial.groundWeights;
            lastDepth = farMaterial.groundBlendDepth;
        }

        assert(lastDepth > material.groundBlendDepth);

        // The patches' tangents run the way the rings' MikkTSpace tangents did (u east, the bitangent's side -1).
        const TerrainRing ring = forestTerrainRings(8.0f).front();
        const IndexedMesh ringMesh = buildIndexedMesh(forestTerrainRingMesh(ring, GroundExtent{forestTerrainCentre, ring.outerHalfSize}));
        const MeshVertex& sample = ringMesh.vertices[ringMesh.vertices.size() / 2];
        assert(sample.tangent.w == -1.0f && sample.tangent.x > 0.5f);

        assert(forest.game.getPillars().size() == 41 && forest.game.getIslands().size() == 8);
        const Transform& ringPlace = forest.world.get<Transform>(forest.game.getRing());
        assert(near(ringPlace.position.x, forestRingPlace.x, 1e-3f) && near(ringPlace.position.z, forestRingPlace.y, 1e-3f) && ringPlace.scale.x == 1.0f);
        assert(!forest.game.getMonolith().empty());

        for (const Entity part : forest.game.getMonolith()) {
            assert(forest.world.get<ModelRenderer>(part).isStatic);
        }

        // On a grid point of the 8 m squares the ground walked on is the land's height; the Monolith's face stops a ray.
        const float x = forestTerrainCentre.x + 24.0f;
        const float z = forestTerrainCentre.y + 40.0f;
        const std::optional<LevelHit> down = raycast(forest.game.getLevel(), Ray{Vec3{x, 500.0f, z}, Vec3{0.0f, -1.0f, 0.0f}});
        assert(down && near(down->hit.point.y, forestHeight(x, z), 0.05f));

        const float level = forestHeight(forestClearingCentre.x, forestClearingCentre.y);
        // Above the glacis the Bastion's first tier stops a ray on its front face or on a fin standing out of it.
        const std::optional<LevelHit> ahead = raycast(forest.game.getLevel(), Ray{Vec3{-13.0f, level + 150.0f, -300.0f}, Vec3{0.0f, 0.0f, -1.0f}});
        const float finDepth = forest.game.getBastionSettings().majorFinDepth;
        assert(ahead && ahead->hit.point.z > forestMonolithPlace.y - 0.01f && ahead->hit.point.z < forestMonolithPlace.y + finDepth + 0.01f);

        const DirectionalLight& sun = forest.world.get<DirectionalLight>(forest.game.getSun());
        assert(sun.castsShadows && sun.intensity > 0.0f && sun.direction.y < 0.0f);
        assert(forest.world.has<Atmosphere>(forest.game.getAir()) && forest.world.has<Fog>(forest.game.getAir()));
        assert(forest.world.get<Fog>(forest.game.getAir()).maxDistance >= 20000.0f);
        assert(forest.game.getCamera().getFarPlane() >= 20480.0f);

        const Entity body = forest.game.getControls().getFirstPersonBody();
        const Vec3 start = forest.world.get<Transform>(body).position;
        assert((Vec2{start.x, start.z} - forestStartPlace).length() < 0.01f);

        for (int tick = 0; tick < 120; ++tick) {
            runFixedUpdate(forest.game, forest.world, 1.0f / 120.0f, tick / 120.0);
        }

        const Vec3 settled = forest.world.get<Transform>(body).position;
        const std::optional<LevelHit> under = raycast(forest.game.getLevel(), Ray{settled + Vec3{0.0f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}});
        assert(under && near(settled.y, under->hit.point.y, 0.1f));
    }

    /*
    * The start among the trees (a start-up setting, for looking at them): the player just south of the tree with the
    * most others round it, facing it (north), where forestTreesStart says.
    */
    void testTheStartAmongTheTrees() {
        ForestValleySettings valley{.groundCoarseness = 2, .pillarCount = 40, .islandCount = 8};
        valley.start = ForestStart::Trees;
        Forest forest{valley};
        const Vec3 start = forest.world.get<Transform>(forest.game.getControls().getFirstPersonBody()).position;
        const Vec2 expected = forestTreesStart(forest.game.getTreePlacements());
        assert((Vec2{start.x, start.z} - expected).length() < 0.01f);
        assert(ForestValleySettings{}.start == ForestStart::Rim);
    }

    /*
    * The valley's layer cake: beds touching from its bottom to its top; soft sandstone beds 15 to 60 m thick, each a tone
    * between the two sandstone tints and a neighbouring soft bed's tone at least 0.3 apart, so each shows; hard beds of
    * blocks 5 to 15 m thick, about a fifth of them and never two together; the same every run; the bed at a height its own.
    */
    void testTheStrataAreOneCake() {
        const std::vector<StrataBand> bands = valleyStrata();
        assert(bands.size() > 30 && bands.front().bottom == valleyStrataBottom && bands.back().top == valleyStrataTop);
        int hardBeds = 0;

        for (std::size_t bed = 0; bed < bands.size(); ++bed) {
            const StrataBand& band = bands[bed];
            const float thickness = band.top - band.bottom;
            const bool last = bed + 1 == bands.size();
            assert(band.tone >= 0.0f && band.tone <= 1.0f);

            if (band.hard) {
                hardBeds += 1;
                assert(thickness <= 15.0f + 1e-3f && (last || thickness >= 5.0f - 1e-3f));
            } else {
                assert(thickness <= 60.0f + 1e-3f && (last || thickness >= 15.0f - 1e-3f));
            }

            if (bed > 0) {
                const StrataBand& below = bands[bed - 1];
                assert(band.bottom == below.top && !(band.hard && below.hard));
            }

            if (bed > 1 && !band.hard) {
                const StrataBand& softBelow = bands[bed - 1].hard ? bands[bed - 2] : bands[bed - 1];
                assert(std::fabs(band.tone - softBelow.tone) >= 0.3f - 1e-6f);
            }

            assert(strataBandAt(bands, 0.5f * (band.bottom + band.top)).bottom == band.bottom);
        }

        const float hardShare = static_cast<float>(hardBeds) / static_cast<float>(bands.size());
        assert(hardShare > 0.1f && hardShare < 0.35f);

        const std::vector<StrataBand> again = valleyStrata();
        assert(again.size() == bands.size() && again[17].top == bands[17].top && again[17].tone == bands[17].tone);
    }

    /*
    * The strip the rock is painted by: one texel tall, each texel's layers adding up to one (within 8-bit rounding) and
    * the moss's channel empty; a texel inside a soft bed mixes the two sandstone tints by its tone, inside a hard bed it
    * is all blocks.
    */
    void testTheStrataStripHoldsTheBeds() {
        const std::vector<StrataBand> bands = valleyStrata();
        const int texels = 1024;
        const ImageData image = strataImage(bands, texels);
        assert(image.width == static_cast<std::uint32_t>(texels) && image.height == 1 && image.encoding == TextureEncoding::Linear);
        assert(image.storage == TextureStorage::Exact);
        const float texelMetres = (valleyStrataTop - valleyStrataBottom) / static_cast<float>(texels);

        for (int texel = 0; texel < texels; ++texel) {
            const std::uint8_t* pixel = &image.pixels[static_cast<std::size_t>(texel) * 4];
            assert(std::abs(pixel[0] + pixel[1] + pixel[2] - 255) <= 2 && pixel[3] == 0);
            const float low = valleyStrataBottom + texelMetres * static_cast<float>(texel);
            const StrataBand& band = strataBandAt(bands, low);

            if (&band == &strataBandAt(bands, low + texelMetres - 1e-3f)) {
                if (band.hard) {
                    assert(pixel[2] == 255);
                } else {
                    assert(std::abs(static_cast<float>(pixel[1]) - band.tone * 255.0f) <= 1.0f && pixel[2] == 0);
                }
            }
        }
    }

    /*
    * The wavy beds' noise: it repeats seamlessly (its last column and row lead into its first as smoothly as any two
    * neighbours do), spans most of its range about the middle, and is the same every time.
    */
    void testTheWarpRepeatsSeamlessly() {
        const int size = 128;
        const ImageData warp = strataWarpImage(size);
        assert(warp.width == static_cast<std::uint32_t>(size) && warp.height == static_cast<std::uint32_t>(size) && warp.encoding == TextureEncoding::Linear);
        const auto at = [&](int x, int y) {
            return static_cast<int>(warp.pixels[(static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)) * 4]);
        };
        int largestStep = 0;
        int lowest = 255;
        int highest = 0;
        long sum = 0;

        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size - 1; ++x) {
                largestStep = std::max(largestStep, std::abs(at(x + 1, y) - at(x, y)));
                lowest = std::min(lowest, at(x, y));
                highest = std::max(highest, at(x, y));
                sum += at(x, y);
            }
        }

        for (int y = 0; y < size; ++y) {
            assert(std::abs(at(0, y) - at(size - 1, y)) <= largestStep + 1);
            assert(std::abs(at(y, 0) - at(y, size - 1)) <= largestStep + 1);
        }

        assert(lowest < 70 && highest > 185);
        const float mean = static_cast<float>(sum) / static_cast<float>(size * (size - 1));
        assert(mean > 105.0f && mean < 150.0f);
        assert(strataWarpImage(size).pixels == warp.pixels);
    }

    // The streaks' texture: mostly clean rock (bright), with dark stains in places, repeating seamlessly too.
    void testStreaksAreMostlyClean() {
        const int size = 128;
        const ImageData streaks = streakImage(size);
        assert(streaks.width == static_cast<std::uint32_t>(size) && streaks.encoding == TextureEncoding::Linear);
        assert(streaks.storage == TextureStorage::Exact && strataWarpImage(8).storage == TextureStorage::Exact && toneVariationImage(8).storage == TextureStorage::Exact);
        int clean = 0;
        int darkest = 255;

        for (std::size_t texel = 0; texel < static_cast<std::size_t>(size * size); ++texel) {
            const int value = streaks.pixels[texel * 4];
            clean += value > 230 ? 1 : 0;
            darkest = std::min(darkest, value);
        }

        assert(clean > size * size / 2 && darkest < 60);
    }

    /*
    * The pillars and islands share the rock: four layers projected in metres (the tile sizes' inverses), painted by the
    * strata strip over the cake's heights, the moss on top; with no downloaded assets, the layers' stand-in colours.
    */
    void testThePillarsAreMadeOfTheStrata() {
        Forest forest;
        assert(forest.game.getRockShapes().size() == forest.game.getPillars().size() + forest.game.getIslands().size());
        const Material& rock = forest.world.get<ModelRenderer>(forest.game.getRockShapes().front()).parts.front().material;
        assert(rock.groundLayers.size() == 4 && rock.groundProjection == GroundProjection::Axes && rock.groundPaint == GroundPaint::Strata && rock.groundWeights.isValid());
        assert(rock.strataBottom == valleyStrataBottom && rock.strataTop == valleyStrataTop && rock.groundTopLayer == forestMossLayer);

        assert(rock.strataWarp.isValid() && rock.strataWarpMetres == forest.game.getRockSettings().warpMetres);
        assert(rock.groundStreaks.isValid() && rock.streakStrength == forest.game.getRockSettings().streakStrength);
        assert(rock.groundHexTiling && rock.groundVariation.isValid() && rock.variationStrength == forest.game.getRockSettings().toneVariation);

        for (std::size_t layer = 0; layer < 4; ++layer) {
            assert(near(rock.groundRepeats[layer], 1.0f / forestRockLayers[layer].tileMetres, 1e-7f));
            const Pixel standIn = forestRockLayers[layer].standIn;
            const Pixel colour = rock.groundLayers[layer].colour;
            assert(colour.r == standIn.r && colour.g == standIn.g && colour.b == standIn.b);
        }

        // The two sandstone tints are the same scan.
        assert(std::string{forestRockLayers[0].folder} == forestRockLayers[1].folder);

        // Each its own shape, drawn only, at its real size (not scaled).
        MeshHandle lastShape;

        for (const Entity shape : forest.game.getRockShapes()) {
            const RenderPart& part = forest.world.get<ModelRenderer>(shape).parts.front();
            assert(part.material.groundWeights == rock.groundWeights);
            assert(forest.assets.isDrawnOnly(part.mesh) && part.mesh != lastShape);
            // Two coarser levels of detail, simplified from it (docs/research/LOD.md): pillars and islands stand hundreds of metres off.
            assert(forest.assets.getMeshLevels().at(part.mesh.index).size() == 2);
            assert(forest.world.get<Transform>(shape).scale.x == 1.0f && forest.world.get<Transform>(shape).scale.y == 1.0f);
            lastShape = part.mesh;
        }
    }

    // forest-view places the camera at a named view and refuses names there are none of.
    void testViewsHaveACommand() {
        Forest forest;
        CommandRegistry registry;
        forest.game.addCommands(registry, forest.world);

        for (const ForestView& view : forestViews()) {
            assert(registry.execute(std::string{"forest-view "} + view.name).ok);
            assert((forest.game.getCamera().getPosition() - view.position).length() < 1e-3f);
        }

        assert(forestViews().size() == 3);
        assert(!registry.execute("forest-view nowhere").ok);
        assert(!registry.execute("forest-view").ok);
    }

    // forest-twigs shows every twig prototype's every level in front of the camera, facing it, and off hides them;
    // they start hidden.
    void testTwigsHaveAStand() {
        Forest forest;
        CommandRegistry registry;
        forest.game.addCommands(registry, forest.world);
        const std::vector<Entity>& stand = forest.game.getTwigStand();
        std::size_t levels = 0;

        for (const TwigPrototype& twig : forest.game.getTwigPrototypes()) {
            levels += twig.levels.size();
        }

        assert(static_cast<int>(forest.game.getTwigPrototypes().size()) == forest.game.getTwigSettings().prototypes);

        // Every tree look has its twigs (forest_trees_test checks them against the cards).
        assert(forest.game.getTreeTwigs().size() == forest.game.getTreeLooks().size());

        for (const std::vector<PlacedTwig>& twigs : forest.game.getTreeTwigs()) {
            assert(!twigs.empty());
        }

        // And as the renderer takes them: every twig in bricks over the prototypes' meshes, each with its levels given.
        assert(forest.game.getTreeTwigSets().size() == forest.game.getTreeTwigs().size());

        for (std::size_t look = 0; look < forest.game.getTreeTwigSets().size(); ++look) {
            const TwigSet& set = *forest.game.getTreeTwigSets()[look];
            assert(set.twigs.size() == forest.game.getTreeTwigs()[look].size() && !set.bricks.empty());
            assert(static_cast<int>(set.prototypes.size()) == forest.game.getTwigSettings().prototypes);

            for (const MeshHandle prototype : set.prototypes) {
                assert(forest.assets.hasClusterLevels(prototype) && forest.assets.getGivenClusterLevels(prototype) != nullptr);
            }

            for (const TwigInstance& twig : set.twigs) {
                assert(twig.prototype < set.prototypes.size());
            }

            // The look draws them as its last part, casting nothing, swaying as its leaves, whose cards stand aside where
            // twigs are drawn; the part's mesh stands for the twigs' box and ends where the leaves do.
            const PlantedKindLook& kind = forest.game.getTreeLooks()[look];
            const ModelRenderer& twigModel = kind.models.back();
            const ModelRenderer& leaves = kind.models.at(treeLeavesModel);
            assert(twigModel.parts.size() == 1 && twigModel.parts.front().twigs.get() == &set && !twigModel.castsShadows);
            assert(leaves.twigFallback && !twigModel.twigFallback && twigModel.sway.kind == leaves.sway.kind);
            const std::vector<MeshLevel>& standLevels = forest.assets.getMeshLevels().at(twigModel.parts.front().mesh.index);
            const std::vector<MeshLevel>& leafLevels = forest.assets.getMeshLevels().at(leaves.parts.front().mesh.index);
            assert(standLevels.size() == 1 && !standLevels.front().mesh.isValid() && standLevels.front().errorMetres == leafLevels.back().errorMetres);
        }
        assert(!stand.empty() && stand.size() == levels);

        for (const Entity entity : stand) {
            assert(!forest.world.get<ModelRenderer>(entity).isVisible());
        }

        assert(registry.execute("forest-twigs").ok);
        const Camera& camera = forest.game.getCamera();

        for (const Entity entity : stand) {
            assert(forest.world.get<ModelRenderer>(entity).isVisible());
            const Transform& place = forest.world.get<Transform>(entity);
            const Vec3 ahead = place.position - camera.getPosition();
            assert(ahead.dot(camera.getForward()) > 1.0f && ahead.length() < 40.0f);
            assert(place.rotation.rotate(Vec3{0.0f, 0.0f, 1.0f}).dot(ahead) < 0.0f);
        }

        assert(registry.execute("forest-twigs off").ok);

        for (const Entity entity : stand) {
            assert(!forest.world.get<ModelRenderer>(entity).isVisible());
        }

        assert(!registry.execute("forest-twigs on").ok);
        assert(!registry.execute("forest-twigs off now").ok);
    }

    // The valley never moves, so all of it is still (its draws kept from frame to frame) but for the drifting rocks.
    void testTheValleyIsStill() {
        Forest forest;
        std::size_t moving = 0;

        forest.world.each<ModelRenderer>([&](Entity entity, const ModelRenderer& renderer) {
            const bool drifts = forest.world.has<DriftingRock>(entity);
            assert(renderer.isStill() == !drifts);
            moving += drifts ? 1 : 0;
        });

        assert(moving > 0);
    }

    // The haze and the ground's blend follow their settings each frame, so set commands and F5 change them at once.
    void testSettingsApplyEachFrame() {
        Forest forest;
        Input input;
        forest.game.getMistSettings().enabled = false;
        assert(forest.world.get<ModelRenderer>(forest.game.getGround()).parts.front().material.groundHexTiling);
        forest.game.getGroundSettings().blendDepth = 0.45f;
        forest.game.getGroundSettings().hexTiling = false;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(!forest.world.has<Fog>(forest.game.getAir()));
        assert(!forest.world.get<ModelRenderer>(forest.game.getGround()).parts.front().material.groundHexTiling);
        const Terrain& terrain = forest.world.get<Terrain>(forest.game.getTerrain());
        assert(!terrain.regions.back().material.groundHexTiling && !terrain.regions.front().material.groundHexTiling);
        const float nearDepth = forest.world.get<ModelRenderer>(forest.game.getGround()).parts.front().material.groundBlendDepth;
        assert(near(nearDepth, forestBlendDepth(0.45f, 4.0f * forestFinestPaintTexel), 1e-6f)); // the tests' coarsest ground
        assert(terrain.regions.front().material.groundBlendDepth == nearDepth);
        const float farDepth = terrain.regions[1].material.groundBlendDepth;
        assert(farDepth > 0.45f && farDepth <= 1.0f);

        forest.game.getRockSettings() = ForestRockSettings{.blendDepth = 0.6f, .mossStart = 0.4f, .mossFull = 0.9f, .warpMetres = 6.0f, .streakStrength = 0.2f,
                                                           .hexTiling = false, .toneVariation = 0.4f};
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);

        for (const Entity shape : forest.game.getRockShapes()) {
            const Material& rock = forest.world.get<ModelRenderer>(shape).parts.front().material;
            assert(rock.groundBlendDepth == 0.6f && rock.groundTopStart == 0.4f && rock.groundTopFull == 0.9f);
            assert(rock.streakStrength == 0.2f && !rock.groundHexTiling && rock.variationStrength == 0.4f);
            assert(forest.world.has<RenderChanged>(shape));
        }

        /*
        * The structures: each one layer with a seam map, read in each facet's plane, painted everywhere by strata (the
        * meshes' texture coordinates are the facets' numbers). The Ring in the stealth coating, the Bastion, its bridge and
        * the far towers in concrete with formwork; each material's settings apply live.
        */
        for (const Entity structure : forest.game.getStructures()) {
            const Material& material = forest.world.get<ModelRenderer>(structure).parts.front().material;
            assert(material.groundLayers.size() == 1 && material.groundProjection == GroundProjection::Facets && material.groundPaint == GroundPaint::Strata);
            assert(material.groundLayers.front().texture.isValid() && material.groundLayers.front().normalTexture.isValid() && material.facetTone > 0.0f);
        }

        assert(forest.game.getMonolith().size() == 2 && forest.game.getStructures().size() == 2 + 1 + 10);
        const Material& ringCoating = forest.world.get<ModelRenderer>(forest.game.getRing()).parts.front().material;
        const Material& concrete = forest.world.get<ModelRenderer>(forest.game.getMonolith().front()).parts.front().material;
        assert(ringCoating.groundLayers.front().texture != concrete.groundLayers.front().texture);
        assert(near(concrete.groundRepeats[0], 1.0f / forest.game.getBastionSettings().formworkTile, 1e-7f));

        /*
        * At night: the Bastion and each far tower carry a band strip of slit windows over their own heights, the bridge
        * none; a light-strip mesh a tower, glowing; the Bastion's 18 floodlights, unshadowed, aimed up its walls.
        */
        const Material& bridge = forest.world.get<ModelRenderer>(forest.game.getMonolith().back()).parts.front().material;
        assert(concrete.windowBands.isValid() && !bridge.windowBands.isValid() && concrete.windowPitch > 0.0f && concrete.windowWarm.x > 0.0f);
        assert(concrete.strataTop - concrete.strataBottom > forest.game.getBastionSettings().height);
        assert(forest.game.getLightStrips().size() == 11 && forest.game.getFloodlights().size() == 18);
        assert(forest.world.get<ModelRenderer>(forest.game.getLightStrips().front()).parts.front().material.emissive.z > 0.0f);

        for (const Entity flood : forest.game.getFloodlights()) {
            const SpotLight& light = forest.world.get<SpotLight>(flood);
            assert(!light.castsShadows && light.intensity == forest.game.getBastionSettings().floodIntensity && light.direction.y > 0.9f);
        }

        forest.game.getBastionSettings().floodIntensity = 900.0f;
        forest.game.getBastionSettings().stripBrightness = 3.0f;
        forest.game.getBastionSettings().windowBrightness = 2.0f;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(forest.world.get<SpotLight>(forest.game.getFloodlights().front()).intensity == 900.0f);
        assert(near(forest.world.get<ModelRenderer>(forest.game.getLightStrips().back()).parts.front().material.emissive.z, 3.0f * bastionCoolLight.z, 1e-6f));
        assert(near(forest.world.get<ModelRenderer>(forest.game.getMonolith().front()).parts.front().material.windowWarm.x, 2.0f * bastionWarmLight.x, 1e-6f));

        /*
        * The obstruction lights: the Bastion's four a level on six levels, red, flashing together at 30 a minute with a
        * spill of light on their corners; each far tower's its own, at 20 to 40 a minute, out of step, with none; their
        * intensity live; flashing by the simulation's clock between ticks.
        */
        const std::vector<Entity>& beacons = forest.game.getBeacons();
        const std::size_t onBastion = forest.game.getBastionBeaconCount();
        assert(onBastion == 24 && beacons.size() > onBastion);
        const Beacon& first = forest.world.get<Beacon>(beacons.front());
        assert(first.colour.x > 10.0f * first.colour.y && near(first.flash.periodSeconds, 2.0f, 1e-5f) && first.spill > 0.0f);
        bool outOfStep = false;

        for (std::size_t index = onBastion; index < beacons.size(); ++index) {
            const Beacon& far = forest.world.get<Beacon>(beacons[index]);
            assert(far.spill == 0.0f && far.flash.periodSeconds >= 1.5f - 1e-4f && far.flash.periodSeconds <= 3.0f + 1e-4f);
            outOfStep = outOfStep || std::fabs(far.flash.periodSeconds - forest.world.get<Beacon>(beacons[onBastion]).flash.periodSeconds) > 0.01f;
        }

        assert(outOfStep);

        // On, a quarter of a second into the Bastion's period (the last tick ending then, alpha 1); dark a second in.
        forest.game.onFixedUpdate(forest.world, 1.0f / 60.0f, 0.25);
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(near(forest.world.get<Beacon>(beacons.front()).level, 1.0f, 1e-5f) && forest.world.get<PointLight>(beacons.front()).intensity > 0.0f);
        assert(!forest.world.has<PointLight>(beacons.back()));
        forest.game.onFixedUpdate(forest.world, 1.0f / 60.0f, 1.0);
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(forest.world.get<Beacon>(beacons.front()).level == 0.0f && forest.world.get<PointLight>(beacons.front()).intensity == 0.0f);

        forest.game.getBastionSettings().beaconIntensity = 500.0f;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(forest.world.get<Beacon>(beacons.front()).intensity == 500.0f && forest.world.get<Beacon>(beacons.back()).intensity == 500.0f);

        /*
        * The flashlight (gameplay/flashlight): off at the start, F switches it on in the right hand and off again, and so
        * does its setting (the set command).
        */
        const Entity flashlight = forest.game.getFlashlight();
        assert(!forest.game.getFlashlightSettings().on && !forest.world.has<SpotLight>(flashlight));
        Input pressF;
        pressF.beginFrame();
        pressF.pressKey(Key::F);
        forest.game.onInput(forest.world, pressF);
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(forest.game.getFlashlightSettings().on && forest.world.has<SpotLight>(flashlight));
        const FlashlightPose held = flashlightInHand(forest.game.getCamera(), forest.game.getFlashlightSettings());
        assert((forest.world.get<Transform>(flashlight).position - held.position).length() < 1e-4f);
        forest.game.getFlashlightSettings().on = false;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(!forest.world.has<SpotLight>(flashlight));

        forest.game.getStructureSettings().seamDepth = 2.0f;
        forest.game.getStructureSettings().seamTile = 32.0f;
        forest.game.getStructureSettings().coatingRoughness = 0.8f;
        forest.game.getBastionSettings().formworkDepth = 1.5f;
        forest.game.getBastionSettings().formworkTile = 33.0f;
        forest.game.getBastionSettings().concreteRoughness = 0.7f;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);

        const Material& ringAfter = forest.world.get<ModelRenderer>(forest.game.getRing()).parts.front().material;
        assert(ringAfter.normalScale == 2.0f && near(ringAfter.groundRepeats[0], 1.0f / 32.0f, 1e-7f) && ringAfter.groundLayers.front().roughness == 0.8f);
        assert(forest.world.has<RenderChanged>(forest.game.getRing()));

        for (const Entity structure : forest.game.getStructures()) {
            if (structure == forest.game.getRing()) {
                continue;
            }

            const Material& after = forest.world.get<ModelRenderer>(structure).parts.front().material;
            assert(after.normalScale == 1.5f && near(after.groundRepeats[0], 1.0f / 33.0f, 1e-7f) && after.groundLayers.front().roughness == 0.7f);
            assert(forest.world.has<RenderChanged>(structure));
        }

        // The ground and the rocks are still: a change marks them to be built again, an unchanged frame marks nothing.
        assert(forest.world.has<RenderChanged>(forest.game.getGround()));
        clearRenderChanged(forest.world);
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(!forest.world.has<RenderChanged>(forest.game.getGround()));

        for (const Entity shape : forest.game.getRockShapes()) {
            assert(!forest.world.has<RenderChanged>(shape));
        }

        forest.game.getMistSettings().enabled = true;
        forest.game.getSunSettings().solarHours = 18.5f;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(forest.world.has<Fog>(forest.game.getAir()));
        assert(forest.world.get<DirectionalLight>(forest.game.getSun()).direction.y > 0.0f);
    }

    /*
    * Two layers of mist: the valley's thin haze, and a dense pool on the floor whose top and soft edge stay under the
    * canopy trees' forks (docs/research/FOGLAYERS.md), each a Fog the frame's lighting collects, the haze first. The
    * floor's switch takes only the pool away; the mist's takes both. The pool is off by default (the user, Nov 19 2026).
    */
    void testTheMistHasAFloorLayer() {
        Forest forest;
        Input input;
        const ForestMistSettings& settings = forest.game.getMistSettings();
        assert(settings.enabled && !settings.floorEnabled);
        assert(settings.fog.shape == FogShape::Exponential && settings.floor.shape == FogShape::Pooled);
        assert(settings.floor.baseHeight + settings.floor.scaleHeight < ForestTreeSettings{}.forkHeight);
        assert(settings.floor.density >= 0.02f && settings.floor.density <= 0.05f);

        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        std::vector<Fog> layers = collectLighting(forest.world, 1.0f).fogLayers;
        assert(layers.size() == 1 && layers[0].shape == FogShape::Exponential);

        forest.game.getMistSettings().floorEnabled = true;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        layers = collectLighting(forest.world, 1.0f).fogLayers;
        assert(layers.size() == 2 && layers[0].shape == FogShape::Exponential && layers[1].shape == FogShape::Pooled);
        assert(layers[1].density == settings.floor.density);

        forest.game.getMistSettings().floor.density = 0.04f;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(collectLighting(forest.world, 1.0f).fogLayers.at(1).density == 0.04f);

        forest.game.getMistSettings().floorEnabled = false;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        layers = collectLighting(forest.world, 1.0f).fogLayers;
        assert(layers.size() == 1 && layers[0].shape == FogShape::Exponential);

        forest.game.getMistSettings().floorEnabled = true;
        forest.game.getMistSettings().enabled = false;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(collectLighting(forest.world, 1.0f).fogLayers.empty());
    }

    /*
    * Cluster levels (ROADMAP phase 21, 2b-ii) only where the cut will use them: the rocks, the drifting rocks, the
    * structures and the trees' bark; not the rocks' old levels, the leaves, the light strips, the terrain or anything
    * else, so exactly those meshes are marked.
    */
    void testClusterLevelsWhereUsed() {
        Forest forest;
        std::set<std::uint32_t> expected;
        const auto expectMarked = [&](MeshHandle mesh) {
            assert(forest.assets.hasClusterLevels(mesh));
            expected.insert(mesh.index);
        };
        const auto firstMesh = [&](Entity entity) { return forest.world.get<ModelRenderer>(entity).parts.front().mesh; };

        for (const Entity rock : forest.game.getRockShapes()) {
            expectMarked(firstMesh(rock));

            for (const MeshLevel& level : forest.assets.getMeshLevels().at(firstMesh(rock).index)) {
                assert(!level.mesh.isValid() || !forest.assets.hasClusterLevels(level.mesh));
            }
        }

        for (const Entity rock : forest.game.getDriftingRocks()) {
            expectMarked(firstMesh(rock));
        }

        for (const Entity structure : forest.game.getStructures()) {
            expectMarked(firstMesh(structure));
        }

        for (const Entity strip : forest.game.getLightStrips()) {
            assert(!forest.assets.hasClusterLevels(firstMesh(strip)));
        }

        assert(!forest.game.getTreeLooks().empty());

        for (const PlantedKindLook& look : forest.game.getTreeLooks()) {
            expectMarked(look.models.at(0).parts.front().mesh);
            assert(!forest.assets.hasClusterLevels(look.models.at(1).parts.front().mesh));
        }

        // The twig prototypes, with their levels given (twigs.h).
        for (const MeshHandle prototype : forest.game.getTreeTwigSets().front()->prototypes) {
            expectMarked(prototype);
        }

        assert(!forest.assets.hasClusterLevels(firstMesh(forest.game.getGround())));
        std::size_t marked = 0;

        for (std::uint32_t mesh = 0; mesh < forest.assets.getMeshCount(); ++mesh) {
            marked += forest.assets.hasClusterLevels(MeshHandle{mesh}) ? 1 : 0;
        }

        assert(marked == expected.size());
    }
}

void testForest() {
    testItIsRegistered();
    testNoiseIsSmoothAndRepeatable();
    testTheValleyHasAFloorWallsAndARim();
    testThePathRunsFromTheRimThroughTheRingToTheClearing();
    testTheRingsMeetWithoutCracks();
    testHardBedsStandOut();
    testJointsAreNarrowVerticalCracks();
    testShapesWaveTheirBedsAsTheShaderDoes();
    testPillarsStandOutAtHardBeds();
    testTheShapesFaceOutwards();
    testSurfacesGoStraightToTheRenderFormat();
    testShapesArePlacedApartAndClearOfTheWalk();
    testTheGroundIsPaintedByPlace();
    testTheWeightsTextureHoldsThePaint();
    testCoarsePaintBlendsSofter();
    testLayerFilesAreFoundByRole();
    testTheGameBuildsTheValley();
    testClusterLevelsWhereUsed();
    testTheStartAmongTheTrees();
    testTheStrataAreOneCake();
    testTheStrataStripHoldsTheBeds();
    testTheWarpRepeatsSeamlessly();
    testStreaksAreMostlyClean();
    testThePillarsAreMadeOfTheStrata();
    testViewsHaveACommand();
    testTwigsHaveAStand();
    testSettingsApplyEachFrame();
    testTheMistHasAFloorLayer();
    testTheValleyIsStill();
}
