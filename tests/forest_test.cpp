#include "app/game_registry.h"
#include "games/forest/forest_game.h"
#include "games/forest/forest_ground.h"
#include "games/forest/forest_terrain.h"
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

    /*
    * A fresh forest, initialised headless with its stand-in ground (an asset folder that does not exist), so the tests
    * neither need the downloaded assets nor spend time decoding them.
    */
    struct Forest {
        World world;
        AssetManager assets;
        ForestGame game{ControlMode::FirstPerson, true, "no-such-folder/forest"};

        Forest() {
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
    }

    /*
    * The land: hills and hollows (the mist pools in the hollows), a level clearing at the origin for the structure, and
    * a path from it to the southern meadow, level across and walkable along.
    */
    void testTheLandHasAClearingHollowsAndAPath() {
        assert(near(forestHeight(0.0f, 0.0f), 0.0f, 0.2f));

        for (float angle = 0.0f; angle < 6.28f; angle += 0.5f) {
            const float x = 0.8f * forestClearingRadius * std::cos(angle);
            const float z = 0.8f * forestClearingRadius * std::sin(angle);
            assert(near(forestHeight(x, z), 0.0f, 0.3f));
            assert(slopeAt(x, z) < 0.02f);
        }

        float lowest = 0.0f;
        float highest = 0.0f;

        for (float x = -forestHalfSize; x <= forestHalfSize; x += 4.0f) {
            for (float z = -forestHalfSize; z <= forestHalfSize; z += 4.0f) {
                lowest = std::min(lowest, forestHeight(x, z));
                highest = std::max(highest, forestHeight(x, z));
            }
        }

        assert(lowest < -3.0f && highest > 6.0f);

        // Along the path: its centre on the path, level across it, no steeper than a walker manages easily.
        const std::vector<Vec2>& path = forestPathPoints();
        assert(path.size() >= 3);

        for (std::size_t point = 0; point + 1 < path.size(); ++point) {
            const Vec2 start = path[point];
            const Vec2 along = path[point + 1] - start;
            const Vec2 across = Vec2{-along.y, along.x} * (1.0f / along.length());

            for (float share = 0.0f; share < 1.0f; share += 0.05f) {
                const Vec2 centre = start + along * share;
                assert(forestPathDistance(centre.x, centre.y) < 0.01f);
                const Vec2 side = centre + across * (0.6f * forestPathHalfWidth);
                assert(near(forestHeight(side.x, side.y), forestHeight(centre.x, centre.y), 0.06f)); // bends included

                const Vec2 ahead = centre + along * (0.5f / along.length());
                const float rise = std::fabs(forestHeight(ahead.x, ahead.y) - forestHeight(centre.x, centre.y)) / 0.5f;
                assert(rise < 0.3f);
            }
        }
    }

    /*
    * The mesh: a (squares + 1)^2 grid of vertices at the land's heights, two triangles a square facing up, its texture
    * coordinates running 0 to 1 across the whole land (the weights texture's, which the layers repeat).
    */
    void testTheMeshFollowsTheLand() {
        const int squares = 16;
        const Mesh mesh = forestTerrainMesh(squares);
        assert(mesh.vertices.size() == static_cast<std::size_t>((squares + 1) * (squares + 1)));
        assert(mesh.triangles.size() == static_cast<std::size_t>(2 * squares * squares));

        for (const Vec4& vertex : mesh.vertices) {
            assert(vertex.x >= -forestHalfSize - 1e-3f && vertex.x <= forestHalfSize + 1e-3f);
            assert(near(vertex.y, forestHeight(vertex.x, vertex.z), 1e-4f));
        }

        for (const Triangle& triangle : mesh.triangles) {
            assert(faceNormal(mesh.vertices[triangle.first], mesh.vertices[triangle.second], mesh.vertices[triangle.third]).y > 0.0f);

            for (std::size_t corner = 0; corner < 3; ++corner) {
                assert(triangle.uvs[corner].has_value() && triangle.normals[corner].has_value());
                assert(triangle.normals[corner]->y > 0.0f);
            }
        }

        const Triangle& first = mesh.triangles.front();
        const Vec4& corner = mesh.vertices[first.first];
        assert(near(first.uvs[0]->x, forestTextureU(corner.x), 1e-6f) && near(first.uvs[0]->y, forestTextureV(corner.z), 1e-6f));
        assert(near(forestTextureU(-forestHalfSize), 0.0f, 1e-6f) && near(forestTextureU(forestHalfSize), 1.0f, 1e-6f));
        assert(near(forestTextureV(-forestHalfSize), 0.0f, 1e-6f) && near(forestTextureV(forestHalfSize), 1.0f, 1e-6f));
    }

    /*
    * The ground's paint: every place's weights add up to one; grass in the clearing and the meadow, mud on the path,
    * mossy rock on steep slopes, leaf litter under the forest elsewhere.
    */
    void testTheGroundIsPaintedByPlace() {
        for (float x = -forestHalfSize; x <= forestHalfSize; x += 13.0f) {
            for (float z = -forestHalfSize; z <= forestHalfSize; z += 13.0f) {
                const GroundWeights paint = forestGroundPaint(x, z);
                assert(near(paint[0] + paint[1] + paint[2] + paint[3], 1.0f, 1e-4f));

                for (const float weight : paint) {
                    assert(weight >= 0.0f && weight <= 1.0f);
                }
            }
        }

        assert(strongestLayer(forestGroundPaint(5.0f, -8.0f)) == forestGrassLayer);
        assert(forestMeadowShare(forestMeadowCentre.x, forestMeadowCentre.y) > 0.9f);
        const Vec2 inMeadow = forestMeadowCentre + Vec2{8.0f, 8.0f}; // off the path, which ends at its centre
        assert(strongestLayer(forestGroundPaint(inMeadow.x, inMeadow.y)) == forestGrassLayer);

        const std::vector<Vec2>& path = forestPathPoints();
        const Vec2 onPath = (path[2] + path[3]) * 0.5f;
        assert(strongestLayer(forestGroundPaint(onPath.x, onPath.y)) == forestMudLayer);

        bool foundSteep = false;
        bool foundForest = false;

        for (float x = -forestHalfSize + 5.0f; x < forestHalfSize - 5.0f; x += 2.0f) {
            for (float z = -forestHalfSize + 5.0f; z < forestHalfSize - 5.0f; z += 2.0f) {
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

    // The weights texture: the paint at each texel's centre, one channel a layer, as linear bytes.
    void testTheWeightsTextureHoldsThePaint() {
        const int size = 32;
        const ImageData image = forestWeightsImage(size);
        assert(image.width == static_cast<std::uint32_t>(size) && image.height == static_cast<std::uint32_t>(size));
        assert(image.encoding == TextureEncoding::Linear && image.pixels.size() == static_cast<std::size_t>(size * size * 4));

        for (int row = 0; row < size; row += 5) {
            for (int column = 0; column < size; column += 3) {
                const float x = forestTexelCentre(column, size);
                const float z = forestTexelCentre(row, size);
                assert(near(forestTextureU(x), (static_cast<float>(column) + 0.5f) / static_cast<float>(size), 1e-6f));
                const GroundWeights paint = forestGroundPaint(x, z);

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
    * The game: one static ground of four layers blended by its weights texture, walked on as drawn; the sun casting
    * shadows through a real sky; the valley mist; the player standing on the ground where they start.
    */
    void testTheGameBuildsTheForest() {
        Forest forest;
        const Entity ground = forest.game.getGround();
        const ModelRenderer& renderer = forest.world.get<ModelRenderer>(ground);
        assert(renderer.isStatic && renderer.parts.size() == 1);

        const Material& material = renderer.parts.front().material;
        assert(material.groundLayers.size() == 4 && material.groundWeights.isValid());

        for (std::size_t layer = 0; layer < 4; ++layer) {
            assert(near(material.groundRepeats[layer], 2.0f * forestHalfSize / forestGroundLayers[layer].tileMetres, 1e-3f));
        }

        const std::optional<LevelHit> hit = raycast(forest.game.getLevel(), Ray{Vec3{3.0f, 50.0f, 7.0f}, Vec3{0.0f, -1.0f, 0.0f}});
        assert(hit && near(hit->hit.point.y, forestHeight(3.0f, 7.0f), 0.05f));

        const DirectionalLight& sun = forest.world.get<DirectionalLight>(forest.game.getSun());
        assert(sun.castsShadows && sun.intensity > 0.0f && sun.direction.y < 0.0f);
        assert(forest.world.has<Atmosphere>(forest.game.getAir()) && forest.world.has<Fog>(forest.game.getAir()));

        const Vec3 start = forest.world.get<Transform>(forest.game.getControls().getFirstPersonBody()).position;
        assert(near(start.y, forestHeight(start.x, start.z), 0.05f));

        for (int tick = 0; tick < 120; ++tick) {
            runFixedUpdate(forest.game, forest.world, 1.0f / 120.0f, tick / 120.0);
        }

        const Vec3 settled = forest.world.get<Transform>(forest.game.getControls().getFirstPersonBody()).position;
        assert(near(settled.y, forestHeight(settled.x, settled.z), 0.1f));
    }

    // The mist and the ground's blend follow their settings each frame, so set commands and F5 change them at once.
    void testSettingsApplyEachFrame() {
        Forest forest;
        Input input;
        forest.game.getMistSettings().enabled = false;
        forest.game.getGroundSettings().blendDepth = 0.45f;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(!forest.world.has<Fog>(forest.game.getAir()));
        assert(near(forest.world.get<ModelRenderer>(forest.game.getGround()).parts.front().material.groundBlendDepth, 0.45f, 0.0f));

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
    testTheLandHasAClearingHollowsAndAPath();
    testTheMeshFollowsTheLand();
    testTheGroundIsPaintedByPlace();
    testTheWeightsTextureHoldsThePaint();
    testLayerFilesAreFoundByRole();
    testTheGameBuildsTheForest();
    testSettingsApplyEachFrame();
}
