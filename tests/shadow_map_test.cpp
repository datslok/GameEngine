#include "render/gpu/shadow_map.h"
#include "scene/camera.h"
#include "scene/lighting.h"

#include <cassert>
#include <cmath>
#include <stdexcept>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance = 0.0001f) {
        return std::abs(actual - expected) < tolerance;
    }

    // Where a world point lands in a tile's image: x and y from -1 to 1 across the tile, depth from 0 (near) to 1 (far).
    Vec3 project(const Mat4& tileMatrix, const Vec3& point) {
        const Vec4 clip = tileMatrix * Vec4{point.x, point.y, point.z, 1.0f};
        return Vec3{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
    }

    // Inside the tile, at least 1.5 texels from its edge, so the 3x3 soft-shadow samples stay on the tile.
    bool landsInside(const Vec3& projected) {
        const float limit = 1.0f - 3.0f / static_cast<float>(largestShadowTileSize);
        return std::abs(projected.x) <= limit && std::abs(projected.y) <= limit && projected.z > 0.0f && projected.z < 1.0f;
    }

    Camera makeTestCamera(const Vec3& position) {
        return Camera{position, position + Vec3{0.0f, -0.5f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.5f, 0.1f, 100.0f};
    }
}

namespace {
    // The face is picked by the direction's largest component, like a cube map: +X, -X, +Y, -Y, +Z, -Z.
    void testFaceFollowsTheLargestAxis() {
        assert(pointShadowFace(Vec3{5.0f, 1.0f, -2.0f}) == 0);
        assert(pointShadowFace(Vec3{-5.0f, 1.0f, 2.0f}) == 1);
        assert(pointShadowFace(Vec3{0.1f, 3.0f, 0.0f}) == 2);
        assert(pointShadowFace(Vec3{0.0f, -3.0f, 0.1f}) == 3);
        assert(pointShadowFace(Vec3{0.0f, 0.0f, 2.0f}) == 4);
        assert(pointShadowFace(Vec3{1.0f, 1.0f, -3.0f}) == 5);
    }

    // Each face is a camera at the light looking along its axis: a point straight ahead lands in the middle of the image.
    void testEachFaceLooksAlongItsAxis() {
        const Vec3 light{1.0f, 2.0f, 3.0f};
        const Vec3 axes[6] = {
            Vec3{1.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f},
            Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f},
            Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, -1.0f}
        };

        for (int face = 0; face < 6; ++face) {
            const Vec3 ahead = project(pointShadowFaceMatrix(light, face, 50.0f), light + axes[face] * 10.0f);

            assert(nearlyEqual(ahead.x, 0.0f) && nearlyEqual(ahead.y, 0.0f));
            assert(ahead.z > 0.0f && ahead.z < 1.0f);
        }
    }

    // Every direction lands on the face chosen for it, and not right at the edge: the faces are a little wider than 90 degrees.
    void testEveryDirectionLandsInsideItsFace() {
        const Vec3 light{0.0f, 1.0f, 0.0f};
        const float steps[5] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};

        for (float x : steps) {
            for (float y : steps) {
                for (float z : steps) {
                    const Vec3 direction{x, y, z};

                    if (direction.lengthSquared() == 0.0f) {
                        continue;
                    }

                    const int face = pointShadowFace(direction);
                    assert(landsInside(project(pointShadowFaceMatrix(light, face, 50.0f), light + direction * 7.0f)));
                }
            }
        }
    }

    // Depth grows with distance from the light, from 0 at the near plane to 1 at the far plane (the light's range).
    void testDepthGrowsWithDistance() {
        const Mat4 face = pointShadowFaceMatrix(Vec3{0.0f, 0.0f, 0.0f}, 0, 50.0f);

        assert(nearlyEqual(project(face, Vec3{shadowNearPlane, 0.0f, 0.0f}).z, 0.0f));
        assert(nearlyEqual(project(face, Vec3{50.0f, 0.0f, 0.0f}).z, 1.0f));
        assert(project(face, Vec3{10.0f, 0.0f, 0.0f}).z < project(face, Vec3{20.0f, 0.0f, 0.0f}).z);
    }
}

namespace {
    // A spotlight's shadow is one perspective view down its beam, just wide enough for its cone.
    void testSpotShadowCoversTheCone() {
        const Vec3 light{0.0f, 3.0f, 0.0f};
        const Vec3 beam = Vec3{0.0f, -1.0f, -1.0f}.normalized();
        const float outerAngle = 0.44f;
        const Mat4 view = spotShadowMatrix(light, Vec3{0.0f, -2.0f, -2.0f}, outerAngle, 30.0f);

        const Vec3 alongBeam = project(view, light + beam * 5.0f);
        assert(nearlyEqual(alongBeam.x, 0.0f) && nearlyEqual(alongBeam.y, 0.0f));

        // Directions on the edge of the cone, all the way round, still land inside the tile.
        const Vec3 side = beam.cross(Vec3{1.0f, 0.0f, 0.0f}).normalized();
        const Vec3 across = beam.cross(side).normalized();

        for (int step = 0; step < 16; ++step) {
            const float around = static_cast<float>(step) * 0.3927f;
            const Vec3 sideways = side * std::cos(around) + across * std::sin(around);
            const Vec3 edge = beam * std::cos(outerAngle) + sideways * std::sin(outerAngle);

            assert(landsInside(project(view, light + edge * 8.0f)));
        }

        // A beam pointing straight down still gets a valid view (the up direction must not be parallel to it).
        const Mat4 down = spotShadowMatrix(light, Vec3{0.0f, -1.0f, 0.0f}, outerAngle, 30.0f);
        assert(landsInside(project(down, Vec3{0.0f, 0.0f, 0.0f})));
    }

    // A directional light has parallel rays, so its shadow is a box (orthographic view) around what the camera sees nearby.
    void testDirectionalShadowCoversTheViewNearTheCamera() {
        const Camera camera = makeTestCamera(Vec3{2.0f, 5.0f, 4.0f});
        const Vec3 sunlight{0.4f, -1.0f, -0.6f};
        const Mat4 view = directionalShadowMatrix(sunlight, camera);

        const Vec3 centre = camera.getPosition() + camera.getForward() * (directionalShadowDistance * 0.5f);
        const float reach = directionalShadowDistance * 0.45f;
        const Vec3 offsets[6] = {
            Vec3{reach, 0.0f, 0.0f}, Vec3{-reach, 0.0f, 0.0f},
            Vec3{0.0f, reach, 0.0f}, Vec3{0.0f, -reach, 0.0f},
            Vec3{0.0f, 0.0f, reach}, Vec3{0.0f, 0.0f, -reach}
        };

        assert(landsInside(project(view, centre)));

        for (const Vec3& offset : offsets) {
            assert(landsInside(project(view, centre + offset)));
        }

        // Things between the view and the sun still cast into it, even well outside the view.
        assert(landsInside(project(view, centre - sunlight.normalized() * 40.0f)));

        // Further from the sun means deeper.
        assert(project(view, centre).z < project(view, centre + sunlight.normalized() * 5.0f).z);
    }

    // The box moves with the camera in whole shadow texels, so shadow edges do not crawl and shimmer as the camera moves.
    void testDirectionalShadowMovesInWholeTexels() {
        const Vec3 sunlight{0.4f, -1.0f, -0.6f};
        const Vec3 fixedPoint{1.0f, 0.0f, -3.0f};
        const float texelsPerUnit = static_cast<float>(largestShadowTileSize) * 0.5f; // projected units -1..1 span the tile

        for (int step = 1; step < 6; ++step) {
            const float nudge = 0.013f * static_cast<float>(step);
            const Vec3 before = project(directionalShadowMatrix(sunlight, makeTestCamera(Vec3{2.0f, 5.0f, 4.0f})), fixedPoint);
            const Vec3 after = project(directionalShadowMatrix(sunlight, makeTestCamera(Vec3{2.0f + nudge, 5.0f, 4.0f - nudge})), fixedPoint);

            const float shiftX = (after.x - before.x) * texelsPerUnit;
            const float shiftY = (after.y - before.y) * texelsPerUnit;

            assert(nearlyEqual(shiftX, std::round(shiftX), 0.01f));
            assert(nearlyEqual(shiftY, std::round(shiftY), 0.01f));
        }
    }

    void testOrthographic() {
        const Mat4 box = Mat4::orthographic(-2.0f, 2.0f, -1.0f, 3.0f, 1.0f, 11.0f);

        // Corners of the box map to the corners of the -1..1 cube; depth follows OpenGL's convention like perspective.
        const Vec4 nearCorner = box * Vec4{-2.0f, -1.0f, -1.0f, 1.0f};
        const Vec4 farCorner = box * Vec4{2.0f, 3.0f, -11.0f, 1.0f};

        assert(nearlyEqual(nearCorner.x, -1.0f) && nearlyEqual(nearCorner.y, -1.0f) && nearlyEqual(nearCorner.z, -1.0f));
        assert(nearlyEqual(farCorner.x, 1.0f) && nearlyEqual(farCorner.y, 1.0f) && nearlyEqual(farCorner.z, 1.0f));

        // No perspective: w stays 1, so size does not change with distance.
        assert(nearlyEqual(farCorner.w, 1.0f));

        // A flipped box is fine: window pixels count down from the top, so bottom = height and top = 0 maps
        // the top-left pixel to the top-left of the screen (-1, 1).
        const Mat4 pixels = Mat4::orthographic(0.0f, 100.0f, 50.0f, 0.0f, -1.0f, 1.0f);
        const Vec4 topLeft = pixels * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        const Vec4 bottomRight = pixels * Vec4{100.0f, 50.0f, 0.0f, 1.0f};
        assert(nearlyEqual(topLeft.x, -1.0f) && nearlyEqual(topLeft.y, 1.0f));
        assert(nearlyEqual(bottomRight.x, 1.0f) && nearlyEqual(bottomRight.y, -1.0f));

        // Only a box with no width, height or depth is rejected.
        bool rejected = false;

        try {
            Mat4::orthographic(1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
        }
        catch (const std::invalid_argument&) {
            rejected = true;
        }

        assert(rejected);
    }
}

namespace {
    PlacedPointLight pointLightAt(const Vec3& position, bool castsShadows, float range = 20.0f) {
        PlacedPointLight placed{position, PointLight{}};
        placed.light.range = range;
        placed.light.castsShadows = castsShadows;
        return placed;
    }

    PlacedSpotLight spotLightAt(const Vec3& position, bool castsShadows, float range = 20.0f) {
        PlacedSpotLight placed{position, SpotLight{}};
        placed.light.range = range;
        placed.light.castsShadows = castsShadows;
        return placed;
    }

    bool overlap(const ShadowTile& a, const ShadowTile& b) {
        return a.x < b.x + b.size && b.x < a.x + a.size && a.y < b.y + b.size && b.y < a.y + a.size;
    }

    // Every tile lies inside the atlas, none overlaps another, and the shader is told the same rectangles in atlas coordinates.
    void assertTilesFitTheAtlas(const ShadowPlan& plan) {
        assert(plan.tileData.size() == plan.tiles.size());

        for (std::size_t i = 0; i < plan.tiles.size(); ++i) {
            const ShadowTile& tile = plan.tiles[i];
            assert(tile.x + tile.size <= shadowAtlasWidth && tile.y + tile.size <= shadowAtlasHeight);
            assert(tile.size == 256 || tile.size == 512 || tile.size == 1024);

            for (std::size_t j = i + 1; j < plan.tiles.size(); ++j) {
                assert(!overlap(tile, plan.tiles[j]));
            }

            assert(nearlyEqual(plan.tileData[i].rect[0], static_cast<float>(tile.x) / static_cast<float>(shadowAtlasWidth)));
            assert(nearlyEqual(plan.tileData[i].rect[1], static_cast<float>(tile.y) / static_cast<float>(shadowAtlasHeight)));
            assert(nearlyEqual(plan.tileData[i].rect[2], static_cast<float>(tile.size) / static_cast<float>(shadowAtlasWidth)));
            assert(nearlyEqual(plan.tileData[i].rect[3], static_cast<float>(tile.size) / static_cast<float>(shadowAtlasHeight)));
        }
    }

    // Every light casts shadows unless told not to.
    void testLightsCastShadowsByDefault() {
        assert(PointLight{}.castsShadows);
        assert(SpotLight{}.castsShadows);
        assert(DirectionalLight{}.castsShadows);
    }

    // A tile gets enough texels for about 0.04 units each where the light's reach ends, as a power of two from 256 to 1024:
    // a lamp needs far fewer than a long flashlight beam.
    void testTileSizeFollowsTheLightsReach() {
        assert(shadowTileSizeFor(1.0f, 5.0f) == 256);   // 10 units across: 250 texels
        assert(shadowTileSizeFor(1.0f, 10.0f) == 512);  // 500
        assert(shadowTileSizeFor(1.0f, 20.0f) == 1024); // 1000
        assert(shadowTileSizeFor(1.0f, 100.0f) == 1024);
        assert(shadowTileSizeFor(1.0f, 0.5f) == 256);
        assert(shadowTileSizeFor(std::tan(0.44f), 50.0f) == 1024);
    }

    // Tiles are numbered directional lights first, then spotlights, then six per point light, each in slot order.
    // Slots count only the lights the shader really gets, so a light that cannot be drawn does not shift them.
    void testTilesAreHandedOut() {
        FrameLighting lighting;

        PlacedPointLight unusable = pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, true);
        unusable.light.range = 0.0f;
        lighting.pointLights.push_back(unusable);

        lighting.pointLights.push_back(pointLightAt(Vec3{1.0f, 0.0f, 0.0f}, true));
        lighting.pointLights.push_back(pointLightAt(Vec3{2.0f, 0.0f, 0.0f}, false));
        lighting.pointLights.push_back(pointLightAt(Vec3{3.0f, 0.0f, 0.0f}, true));
        lighting.spotLights.push_back(spotLightAt(Vec3{0.0f, 3.0f, 0.0f}, true));
        lighting.directionalLights.push_back(DirectionalLight{});

        const ShadowPlan plan = planShadows(lighting, makeTestCamera(Vec3{0.0f, 2.0f, 5.0f}));

        assert(plan.uniforms.directionalTiles[0] == 0);
        assert(plan.uniforms.spotTiles[0][0] == 1);
        assert(plan.uniforms.spotTiles[0][1] == -1);
        assert(plan.uniforms.pointTiles[0][0] == 2);
        assert(plan.uniforms.pointTiles[0][1] == -1);
        assert(plan.uniforms.pointTiles[0][2] == 8);
        assert(plan.uniforms.pointTiles[0][3] == -1);
        assert(plan.tiles.size() == 14);
        assertTilesFitTheAtlas(plan);

        // The shader gets the same matrices the shadow pass draws with, column-major.
        const Mat4 secondFace = pointShadowFaceMatrix(Vec3{1.0f, 0.0f, 0.0f}, 1, 20.0f, plan.tiles[3].size);

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                assert(nearlyEqual(plan.tiles[3].matrix.values[row][column], secondFace.values[row][column]));
                assert(nearlyEqual(plan.tileData[3].matrix[column * 4 + row], secondFace.values[row][column]));
            }
        }

        // Perspective tiles offset by distance from the light; the directional tile by a fixed amount.
        assert(plan.tileData[2].offset[0] > 0.0f);
        assert(plan.tileData[1].offset[0] > 0.0f);
        assert(plan.tileData[0].offset[0] == 0.0f && plan.tileData[0].offset[1] > 0.0f);

        // A directional light's box always gets a full-size tile.
        assert(plan.tiles[0].size == largestShadowTileSize);
    }

    // There is room for 16 shadowed point lights; the rest still light the scene, without shadows.
    void testShadowedLightsAreCapped() {
        FrameLighting lighting;

        for (int i = 0; i < 20; ++i) {
            lighting.pointLights.push_back(pointLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, true));
        }

        const ShadowPlan plan = planShadows(lighting, makeTestCamera(Vec3{0.0f, 2.0f, 5.0f}));

        assert(maxShadowedPointLights == 16 && maxShadowedSpotLights == 8);
        assert(plan.tiles.size() == 96);
        assert(plan.uniforms.pointTiles[3][3] == 90);
        assert(plan.uniforms.pointTiles[4][0] == -1);
        assertTilesFitTheAtlas(plan);
    }

    // When the lights ask for more than the atlas holds, the least important (latest slots) shrink first, and a point
    // light's six faces always share one size.
    void testTilesShrinkToFit() {
        FrameLighting lighting;

        for (int i = 0; i < 4; ++i) {
            lighting.directionalLights.push_back(DirectionalLight{});
        }

        for (int i = 0; i < 8; ++i) {
            lighting.spotLights.push_back(spotLightAt(Vec3{static_cast<float>(i), 3.0f, 0.0f}, true, 50.0f));
        }

        for (int i = 0; i < 16; ++i) {
            lighting.pointLights.push_back(pointLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, true, 40.0f));
        }

        const ShadowPlan plan = planShadows(lighting, makeTestCamera(Vec3{0.0f, 2.0f, 5.0f}));

        assert(plan.tiles.size() == 4 + 8 + 96);
        assertTilesFitTheAtlas(plan);

        for (int i = 0; i < 4; ++i) {
            assert(plan.tiles[static_cast<std::size_t>(plan.uniforms.directionalTiles[i])].size == largestShadowTileSize);
        }

        std::uint32_t previousSize = largestShadowTileSize;

        for (int slot = 0; slot < 16; ++slot) {
            const std::size_t first = static_cast<std::size_t>(plan.uniforms.pointTiles[slot / 4][slot % 4]);
            const std::uint32_t size = plan.tiles[first].size;

            for (std::size_t face = 1; face < 6; ++face) {
                assert(plan.tiles[first + face].size == size);
            }

            assert(size <= previousSize);
            previousSize = size;
        }

        // The most important point light keeps the size it asked for; the least important was shrunk.
        assert(plan.tiles[static_cast<std::size_t>(plan.uniforms.pointTiles[0][0])].size == largestShadowTileSize);
        assert(previousSize < largestShadowTileSize);
    }

    // Small lights get small tiles, so sixteen lamps take a small corner of the atlas.
    void testSmallLightsGetSmallTiles() {
        FrameLighting lighting;

        for (int i = 0; i < 16; ++i) {
            lighting.pointLights.push_back(pointLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, true, 5.0f));
        }

        const ShadowPlan plan = planShadows(lighting, makeTestCamera(Vec3{0.0f, 2.0f, 5.0f}));

        assert(plan.tiles.size() == 96);
        assertTilesFitTheAtlas(plan);

        for (const ShadowTile& tile : plan.tiles) {
            assert(tile.size == smallestShadowTileSize);
        }
    }

    // With nothing casting shadows there is nothing to draw and every slot says "none".
    void testNoShadowsWithoutLights() {
        const ShadowPlan plan = planShadows(FrameLighting{}, makeTestCamera(Vec3{0.0f, 2.0f, 5.0f}));

        assert(plan.tiles.empty());
        assert(plan.uniforms.spotTiles[1][3] == -1 && plan.uniforms.directionalTiles[3] == -1 && plan.uniforms.pointTiles[15][3] == -1);
    }

    // A slot's tiles are found at [slot / 4][slot % 4] however far down the list it is.
    void testShadowsAreFoundForAnySlot() {
        FrameLighting lighting;

        for (int i = 0; i < 20; ++i) {
            lighting.pointLights.push_back(pointLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, i == 19));
        }

        for (int i = 0; i < 8; ++i) {
            lighting.spotLights.push_back(spotLightAt(Vec3{static_cast<float>(i), 3.0f, 0.0f}, i != 0));
        }

        const ShadowPlan plan = planShadows(lighting, makeTestCamera(Vec3{0.0f, 2.0f, 5.0f}));

        // Spot slot 0 casts none; slots 1 to 7 take one tile each.
        assert(plan.uniforms.spotTiles[0][0] == -1);
        assert(plan.uniforms.spotTiles[0][1] == 0);
        assert(plan.uniforms.spotTiles[1][3] == 6);

        assert(plan.uniforms.pointTiles[4][3] == 7);
        assert(plan.uniforms.pointTiles[4][2] == -1);
        assert(plan.tiles.size() == 13);
    }
}

namespace {
    // A shadow fading in or out reaches the shader as a strength per light slot: 1 full, 0 none.
    void testShadowStrengthIsPassed() {
        FrameLighting lighting;
        PlacedPointLight point = pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, true);
        point.shadowFade = 0.3f;
        lighting.pointLights.push_back(point);
        lighting.pointLights.push_back(pointLightAt(Vec3{1.0f, 0.0f, 0.0f}, false));

        PlacedSpotLight spot = spotLightAt(Vec3{0.0f, 3.0f, 0.0f}, true);
        spot.shadowFade = 0.7f;
        lighting.spotLights.push_back(spot);

        const ShadowPlan plan = planShadows(lighting, makeTestCamera(Vec3{0.0f, 2.0f, 5.0f}));

        assert(nearlyEqual(plan.uniforms.pointShadowStrengths[0][0], 0.3f));
        assert(nearlyEqual(plan.uniforms.pointShadowStrengths[0][1], 0.0f));
        assert(nearlyEqual(plan.uniforms.spotShadowStrengths[0][0], 0.7f));
    }
}

void testShadowMap() {
    testShadowStrengthIsPassed();
    testFaceFollowsTheLargestAxis();
    testEachFaceLooksAlongItsAxis();
    testEveryDirectionLandsInsideItsFace();
    testDepthGrowsWithDistance();
    testOrthographic();
    testSpotShadowCoversTheCone();
    testDirectionalShadowCoversTheViewNearTheCamera();
    testDirectionalShadowMovesInWholeTexels();
    testLightsCastShadowsByDefault();
    testTileSizeFollowsTheLightsReach();
    testTilesAreHandedOut();
    testShadowedLightsAreCapped();
    testTilesShrinkToFit();
    testSmallLightsGetSmallTiles();
    testNoShadowsWithoutLights();
    testShadowsAreFoundForAnySlot();
}
