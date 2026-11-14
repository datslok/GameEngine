#include "app/game_registry.h"
#include "core/command_registry.h"
#include "games/forest/forest_game.h"
#include "games/forest/forest_ground.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/valley_shapes.h"
#include "physics/static_level.h"
#include "render/gpu/ground_layers.h"
#include "scene/atmosphere.h"
#include "scene/fog.h"
#include "scene/light.h"
#include "math/transform.h"
#include "scene/model_renderer.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
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

    /*
    * A fresh valley, initialised headless with its stand-in ground (an asset folder that does not exist) and its
    * coarsest ground and fewer shapes, so the tests neither need the downloaded assets nor spend long building it.
    */
    struct Forest {
        World world;
        AssetManager assets;
        ForestGame game{ControlMode::FirstPerson, true, "no-such-folder/forest"};

        Forest() {
            game.getValleySettings() = ForestValleySettings{.groundCoarseness = 2, .pillarCount = 40, .islandCount = 8};
            game.onInit(world, assets);
        }
    };

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

    // A wrapped surface needs enough rows and columns; a pillar, an island and the Ring face outwards with smooth normals.
    void testTheShapesFaceOutwards() {
        bool refused = false;

        try {
            wrappedSurfaceMesh(1, 2, false, [](int, int) { return Vec3{0.0f, 0.0f, 0.0f}; });
        } catch (const std::invalid_argument&) {
            refused = true;
        }

        assert(refused);

        const int segments = 24;
        const Mesh pillar = pillarMesh(3u, 40, segments);
        checkFacesOutFromRows(pillar, static_cast<std::size_t>(segments));
        float lowest = 1.0f;
        float highest = 0.0f;

        for (const Vec4& point : pillar.vertices) {
            lowest = std::min(lowest, point.y);
            highest = std::max(highest, point.y);
        }

        assert(near(lowest, -pillarBuriedDepth, 1e-4f) && near(highest, 1.0f, 1e-3f));
        assert(pillarMesh(3u, 40, segments).vertices[100].x == pillar.vertices[100].x);
        assert(pillarMesh(4u, 40, segments).vertices[100].x != pillar.vertices[100].x);

        const Mesh island = islandMesh(2u, 28, 32);
        checkFacesOutFromRows(island, 32);
        lowest = 0.0f;
        highest = -1.0f;

        for (const Vec4& point : island.vertices) {
            lowest = std::min(lowest, point.y);
            highest = std::max(highest, point.y);
        }

        assert(near(lowest, -islandDepth, 1e-4f) && highest > 0.0f && highest < 0.15f);

        const float tube = 0.06f;
        const Mesh ring = ringMesh(tube, 48, 12);
        assert(ring.triangles.size() == static_cast<std::size_t>(2 * 48 * 12));

        for (const Triangle& triangle : ring.triangles) {
            const Vec3 middle = centroid(ring, triangle);
            const Vec3 onCircle = Vec3{middle.x, middle.y, 0.0f}.normalized();
            const Vec3 normal = faceNormal(ring.vertices[triangle.first], ring.vertices[triangle.second], ring.vertices[triangle.third]);
            assert(normal.dot(middle - onCircle) > 0.0f);
            assert(near((middle - onCircle).length(), tube, 0.01f));
        }
    }

    /*
    * The pillars and islands: as many as asked, the same every run, spaced apart, at their sizes, clear of the path,
    * the Ring, the clearing and the start; the islands above the pillars under them. The Monolith stands on the clearing
    * and its bridge reaches its pillar.
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
            assert(pillar.variant >= 0 && pillar.variant < layout.variants);

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
        const std::vector<ValleyBox> monolith = monolithBoxes(level);
        float top = 0.0f;

        for (const ValleyBox& box : monolith) {
            top = std::max(top, box.centre.y + box.halfSize.y);
        }

        assert(near(monolith.front().centre.y + monolith.front().halfSize.y, level + 600.0f, 1e-3f));
        assert(monolith.front().centre.y - monolith.front().halfSize.y < level);
        assert(near(monolith.front().centre.z + monolith.front().halfSize.z, forestMonolithPlace.y, 1e-3f));
        assert(top > level + 700.0f);

        const PillarPlacement bridgePillar = monolithPillar(level);
        assert(bridgePillar.foot.y + bridgePillar.height > level + 340.0f);
        assert(farTowerBoxes().size() >= 10);
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

        assert(forest.game.getFarGround().size() == 3);
        TextureHandle lastWeights = material.groundWeights;
        float lastDepth = material.groundBlendDepth;

        for (const Entity far : forest.game.getFarGround()) {
            const ModelRenderer& farRenderer = forest.world.get<ModelRenderer>(far);
            const Material& farMaterial = farRenderer.parts.front().material;
            assert(!farRenderer.isStatic);
            assert(farMaterial.groundWeights.isValid() && farMaterial.groundWeights != lastWeights);
            assert(farMaterial.groundBlendDepth >= lastDepth);
            lastWeights = farMaterial.groundWeights;
            lastDepth = farMaterial.groundBlendDepth;
        }

        assert(lastDepth > material.groundBlendDepth);

        assert(forest.game.getPillars().size() == 41 && forest.game.getIslands().size() == 8);
        assert(near(forest.world.get<Transform>(forest.game.getRing()).scale.x, forestRingRadius, 1e-3f));
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
        const std::optional<LevelHit> ahead = raycast(forest.game.getLevel(), Ray{Vec3{20.0f, level + 50.0f, -300.0f}, Vec3{0.0f, 0.0f, -1.0f}});
        assert(ahead && near(ahead->hit.point.z, forestMonolithPlace.y, 0.05f));

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

    // The haze and the ground's blend follow their settings each frame, so set commands and F5 change them at once.
    void testSettingsApplyEachFrame() {
        Forest forest;
        Input input;
        forest.game.getMistSettings().enabled = false;
        forest.game.getGroundSettings().blendDepth = 0.45f;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(!forest.world.has<Fog>(forest.game.getAir()));
        const float nearDepth = forest.world.get<ModelRenderer>(forest.game.getGround()).parts.front().material.groundBlendDepth;
        assert(near(nearDepth, forestBlendDepth(0.45f, 4.0f * forestFinestPaintTexel), 1e-6f)); // the tests' coarsest ground
        const float farDepth = forest.world.get<ModelRenderer>(forest.game.getFarGround().front()).parts.front().material.groundBlendDepth;
        assert(farDepth > 0.45f && farDepth <= 1.0f);

        forest.game.getMistSettings().enabled = true;
        forest.game.getSunSettings().solarHours = 18.5f;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(forest.world.has<Fog>(forest.game.getAir()));
        assert(forest.world.get<DirectionalLight>(forest.game.getSun()).direction.y > 0.0f);
    }
}

void testForest() {
    testItIsRegistered();
    testNoiseIsSmoothAndRepeatable();
    testTheValleyHasAFloorWallsAndARim();
    testThePathRunsFromTheRimThroughTheRingToTheClearing();
    testTheRingsMeetWithoutCracks();
    testTheShapesFaceOutwards();
    testShapesArePlacedApartAndClearOfTheWalk();
    testTheGroundIsPaintedByPlace();
    testTheWeightsTextureHoldsThePaint();
    testCoarsePaintBlendsSofter();
    testLayerFilesAreFoundByRole();
    testTheGameBuildsTheValley();
    testViewsHaveACommand();
    testSettingsApplyEachFrame();
}
