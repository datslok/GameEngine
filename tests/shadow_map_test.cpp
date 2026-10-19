#include "render/gpu/shadow_map.h"
#include "scene/camera.h"
#include "scene/lighting.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance = 0.0001f) {
        return std::abs(actual - expected) < tolerance;
    }

    // Where a world point lands in a tile's image: x and y from -1 to 1 across the tile, depth from 1 (near) to 0 (far).
    Vec3 project(const Mat4& tileMatrix, const Vec3& point) {
        const Vec4 clip = tileMatrix * Vec4{point.x, point.y, point.z, 1.0f};
        return Vec3{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
    }

    // Inside the tile, at least 1.5 texels from its edge, so the 3x3 soft-shadow samples stay on the tile.
    bool landsInside(const Vec3& projected) {
        const float limit = 1.0f - 3.0f / static_cast<float>(standardShadowTileSize);
        return std::abs(projected.x) <= limit && std::abs(projected.y) <= limit && projected.z > 0.0f && projected.z < 1.0f;
    }

    Camera makeTestCamera(const Vec3& position) {
        return Camera{position, position + Vec3{0.0f, -0.5f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.5f, 0.1f, 100.0f};
    }

    // High above the test lights (spread along x from 0 to 19) looking straight down, far enough out that every cube
    // face of every light overlaps its view, so no face is left out.
    Camera makeOverviewCamera() {
        return Camera{Vec3{10.0f, 60.0f, 0.0f}, Vec3{10.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, 1.5f, 1.5f, 0.1f, 200.0f};
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

    // Depth shrinks with distance from the light (reversed-Z), from 1 at the near plane to 0 at the far plane (the light's range).
    void testDepthShrinksWithDistance() {
        const Mat4 face = pointShadowFaceMatrix(Vec3{0.0f, 0.0f, 0.0f}, 0, 50.0f);

        assert(nearlyEqual(project(face, Vec3{shadowNearPlane, 0.0f, 0.0f}).z, 1.0f));
        assert(nearlyEqual(project(face, Vec3{50.0f, 0.0f, 0.0f}).z, 0.0f));
        assert(project(face, Vec3{10.0f, 0.0f, 0.0f}).z > project(face, Vec3{20.0f, 0.0f, 0.0f}).z);
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

    // The sun gets one tile per cascade, in a row from its first tile: box views, the near ones finer, each the largest size.
    void testSunGetsOneTilePerCascade() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.4f, -1.0f, -0.6f}});
        const Camera camera = makeTestCamera(Vec3{2.0f, 5.0f, 4.0f});
        const ShadowSettings settings;
        const ShadowPlan plan = planShadows(lighting, camera, settings);

        assert(settings.cascadeCount == maxShadowCascades);
        assert(plan.directional[0].firstTile == 0);
        assert(plan.directional[0].cascades == settings.cascadeCount);
        assert(plan.tiles.size() == static_cast<std::size_t>(settings.cascadeCount));

        for (std::size_t cascade = 0; cascade < plan.tiles.size(); ++cascade) {
            assert(plan.tiles[cascade].size == cascadeTilePixels(settings.cascadeResolution));

            if (cascade > 0) {
                assert(plan.tileData[cascade].lens[2] > plan.tileData[cascade - 1].lens[2]);
            }
        }

        ShadowSettings two;
        two.cascadeCount = 2;
        const ShadowPlan twoCascades = planShadows(lighting, camera, two);
        assert(twoCascades.tiles.size() == 2 && twoCascades.directional[0].cascades == 2);
    }

    /*
    * Up close is where shadow texels show most. With a typical view (70 degrees high, 16:9) and the default splits, the
    * first cascade reaches only a few metres and one of its texels covers under a centimetre, while the last still ends
    * at the shadow distance.
    */
    void testNearShadowsAreFine() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.4f, -1.0f, -0.6f}});
        const Camera camera{Vec3{0.0f, 1.7f, 0.0f}, Vec3{0.0f, 1.7f, -10.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.2217f, 16.0f / 9.0f, 0.1f, 1000.0f};
        const ShadowSettings settings;
        const ShadowPlan plan = planShadows(lighting, camera, settings);

        const std::vector<float> splits = cascadeSplits(camera.getNearPlane(), settings.directionalDistance, settings.cascadeCount, settings.cascadeSplitLambda);
        assert(splits[1] < 4.0f);
        assert(plan.tileData[0].lens[2] < 0.01f);
    }

    // Every point the camera sees, out to the shadow distance (or its far plane, if nearer), lands inside some cascade's box.
    void testCascadesCoverTheViewToTheShadowDistance() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.4f, -1.0f, -0.6f}});
        const Camera camera = makeTestCamera(Vec3{2.0f, 5.0f, 4.0f});
        const ShadowSettings settings;
        const ShadowPlan plan = planShadows(lighting, camera, settings);

        const float halfHeight = std::tan(camera.getVerticalFov() * 0.5f);
        const float halfWidth = halfHeight * camera.getAspectRatio();
        const Vec3 viewUp = camera.getRight().cross(camera.getForward());

        for (int step = 1; step <= 40; ++step) {
            const float distance = std::min(settings.directionalDistance, camera.getFarPlane()) * static_cast<float>(step) / 40.0f;

            for (float x : {-0.99f, 0.0f, 0.99f}) {
                for (float y : {-0.99f, 0.0f, 0.99f}) {
                    const Vec3 point = camera.getPosition() + camera.getForward() * distance +
                                       camera.getRight() * (x * halfWidth * distance) + viewUp * (y * halfHeight * distance);
                    bool covered = false;

                    for (const ShadowTile& tile : plan.tiles) {
                        const Vec3 projected = project(tile.matrix, point);
                        covered = covered || (std::abs(projected.x) <= 1.0f && std::abs(projected.y) <= 1.0f &&
                                              projected.z > 0.0f && projected.z < 1.0f);
                    }

                    assert(covered);
                }
            }
        }
    }

    /*
    * Fitted to the depths on screen (sample distribution shadow maps), the cascades end where the farthest surface is
    * instead of at the shadow distance: every cascade's box shrinks, so each texel covers less, and the view is still
    * covered out to the fitted distance. A fitted distance past the settings' changes nothing.
    */
    void testFittedCascadesEndAtTheFittedDistance() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.4f, -1.0f, -0.6f}});
        const Camera camera = makeTestCamera(Vec3{2.0f, 5.0f, 4.0f});
        const ShadowSettings settings;
        const float fitted = 9.3f;

        const ShadowPlan full = planShadows(lighting, camera, settings);
        const ShadowPlan close = planShadows(lighting, camera, settings, fitted);

        for (std::size_t cascade = 0; cascade < full.tiles.size(); ++cascade) {
            assert(close.tileData[cascade].lens[2] < full.tileData[cascade].lens[2]);
            assert(close.tileData[cascade].lens[3] < full.tileData[cascade].lens[3]);
        }

        // The last cascade's sphere holds the slice's far end: about the fitted distance times the view's spread.
        const float halfHeight = std::tan(camera.getVerticalFov() * 0.5f);
        const float spread = std::sqrt(halfHeight * halfHeight * (1.0f + camera.getAspectRatio() * camera.getAspectRatio()));
        assert(close.tileData[close.tiles.size() - 1].lens[3] < fitted * (spread + 1.0f));

        for (int step = 1; step <= 20; ++step) {
            const Vec3 point = camera.getPosition() + camera.getForward() * (fitted * static_cast<float>(step) / 20.0f);
            bool covered = false;

            for (const ShadowTile& tile : close.tiles) {
                const Vec3 projected = project(tile.matrix, point);
                covered = covered || (std::abs(projected.x) <= 1.0f && std::abs(projected.y) <= 1.0f && projected.z > 0.0f && projected.z < 1.0f);
            }

            assert(covered);
        }

        const ShadowPlan beyond = planShadows(lighting, camera, settings, 5000.0f);

        for (std::size_t cascade = 0; cascade < full.tiles.size(); ++cascade) {
            for (int value = 0; value < 16; ++value) {
                assert(beyond.tileData[cascade].matrix[value] == full.tileData[cascade].matrix[value]);
            }
        }
    }

    // Only the brightest directional light (the first slot) gets cascades; the others still light, unshadowed.
    void testOnlyTheFirstDirectionalLightCastsShadows() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{});
        lighting.directionalLights.push_back(DirectionalLight{Vec3{1.0f, -1.0f, 0.0f}});
        const ShadowPlan plan = planShadows(lighting, makeTestCamera(Vec3{2.0f, 5.0f, 4.0f}));

        assert(maxShadowedDirectionalLights == 1);
        assert(plan.directional[0].firstTile == 0);
        assert(plan.directional[1].firstTile == -1 && plan.directional[1].cascades == 0);
    }

    // Each cascade follows the camera in whole texels of its own, so shadow edges do not crawl as the camera moves.
    void testCascadesMoveInWholeTexels() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.4f, -1.0f, -0.6f}});
        const Vec3 fixedPoint{1.0f, 0.0f, -3.0f};
        const ShadowPlan before = planShadows(lighting, makeTestCamera(Vec3{2.0f, 5.0f, 4.0f}));

        for (int step = 1; step < 6; ++step) {
            const float nudge = 0.013f * static_cast<float>(step);
            const ShadowPlan after = planShadows(lighting, makeTestCamera(Vec3{2.0f + nudge, 5.0f, 4.0f - nudge}));

            for (std::size_t cascade = 0; cascade < before.tiles.size(); ++cascade) {
                const float texelsPerUnit = static_cast<float>(before.tiles[cascade].size) * 0.5f;
                const Vec3 then = project(before.tiles[cascade].matrix, fixedPoint);
                const Vec3 now = project(after.tiles[cascade].matrix, fixedPoint);
                const float shiftX = (now.x - then.x) * texelsPerUnit;
                const float shiftY = (now.y - then.y) * texelsPerUnit;

                assert(nearlyEqual(shiftX, std::round(shiftX), 0.02f));
                assert(nearlyEqual(shiftY, std::round(shiftY), 0.02f));
            }
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
    // Tiles left out (size 0) take no room and tell the shader an empty rectangle.
    void assertTilesFitTheAtlas(const ShadowPlan& plan, const ShadowAtlasSize& atlas = shadowAtlasSizeFor(ShadowSettings{})) {
        const float width = static_cast<float>(atlas.width);
        const float height = static_cast<float>(atlas.height);

        assert(plan.tileData.size() == plan.tiles.size());

        for (std::size_t i = 0; i < plan.tiles.size(); ++i) {
            const ShadowTile& tile = plan.tiles[i];

            if (tile.size == 0) {
                assert(plan.tileData[i].rect[2] == 0.0f && plan.tileData[i].rect[3] == 0.0f);
                continue;
            }

            assert(tile.x + tile.size <= atlas.width && tile.y + tile.size <= atlas.height);
            assert(tile.size == 256 || tile.size == 512 || tile.size == 1024 || tile.size == 2048 || tile.size == 4096);

            for (std::size_t j = i + 1; j < plan.tiles.size(); ++j) {
                assert(!overlap(tile, plan.tiles[j]));
            }

            assert(nearlyEqual(plan.tileData[i].rect[0], static_cast<float>(tile.x) / width));
            assert(nearlyEqual(plan.tileData[i].rect[1], static_cast<float>(tile.y) / height));
            assert(nearlyEqual(plan.tileData[i].rect[2], static_cast<float>(tile.size) / width));
            assert(nearlyEqual(plan.tileData[i].rect[3], static_cast<float>(tile.size) / height));
        }
    }

    // Every light casts shadows unless told not to.
    void testLightsCastShadowsByDefault() {
        assert(PointLight{}.castsShadows);
        assert(SpotLight{}.castsShadows);
        assert(DirectionalLight{}.castsShadows);
    }

    // A tile gets enough texels for one to cover about the texel target where the light's reach ends, as a power of two
    // from 256 up to the largest size allowed: a lamp needs far fewer than a long flashlight beam.
    void testTileSizeFollowsTheLightsReach() {
        const float target = ShadowSettings{}.texelTarget;
        assert(target == 0.04f);
        assert(shadowTileSizeFor(1.0f, 5.0f, target, standardShadowTileSize) == 256);   // 10 units across: 250 texels
        assert(shadowTileSizeFor(1.0f, 10.0f, target, standardShadowTileSize) == 512);  // 500
        assert(shadowTileSizeFor(1.0f, 20.0f, target, standardShadowTileSize) == 1024); // 1000
        assert(shadowTileSizeFor(1.0f, 100.0f, target, standardShadowTileSize) == 1024);
        assert(shadowTileSizeFor(1.0f, 0.5f, target, standardShadowTileSize) == 256);

        // Allowed to, a long reach goes on to the largest size: 1500 texels needs 2048.
        assert(largestShadowTileSize == 2048 && standardShadowTileSize == 1024);
        assert(shadowTileSizeFor(1.0f, 30.0f, target, largestShadowTileSize) == 2048);
        assert(shadowTileSizeFor(1.0f, 100.0f, target, largestShadowTileSize) == 2048);

        // A coarser texel target needs half the texels: 40 units across at 0.08 each is 500, and 20 is 250.
        assert(shadowTileSizeFor(1.0f, 20.0f, 0.08f, standardShadowTileSize) == 512);
        assert(shadowTileSizeFor(1.0f, 10.0f, 0.08f, standardShadowTileSize) == 256);
    }

    // Spotlights aim for finer texels than point lights (spotTexelTarget, 0.02): there are few of them, they usually stand
    // still (so their cached tiles cost nothing), and their beams land on the ground at a slant far short of their reach,
    // which stretches each texel. A floodlight (a 35 degree cone reaching 25) gets the largest tile, a default spotlight
    // (25 degrees, reaching 10) 512.
    void testSpotlightsGetFinerTiles() {
        assert(ShadowSettings{}.spotTexelTarget == 0.02f);

        FrameLighting lighting;
        PlacedSpotLight floodlight = spotLightAt(Vec3{0.0f, 8.0f, 0.0f}, true, 25.0f);
        floodlight.light.outerAngle = 35.0f * 3.14159265f / 180.0f;
        lighting.spotLights.push_back(floodlight);
        lighting.spotLights.push_back(spotLightAt(Vec3{3.0f, 2.0f, 0.0f}, true, 10.0f));

        const ShadowPlan plan = planShadows(lighting, makeOverviewCamera());

        assert(plan.tiles.size() == 2);
        assert(plan.tiles[static_cast<std::size_t>(plan.spots[0].firstTile)].size == 2048);
        assert(plan.tiles[static_cast<std::size_t>(plan.spots[1].firstTile)].size == 512);
        assertTilesFitTheAtlas(plan);

        // The shader and the shadow pass agree on the large tile's matrix.
        const Mat4 expected = spotShadowMatrix(floodlight.position, floodlight.light.direction, floodlight.light.outerAngle,
                                               floodlight.light.range, 2048);
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                assert(nearlyEqual(plan.tiles[0].matrix.values[row][column], expected.values[row][column]));
            }
        }
    }

    // When the atlas is full, spotlights only give up their large tiles once every point light is down to the smallest
    // size, least important spotlight first; the most important keeps its 2048 tile.
    void testSpotTilesShrinkAfterPointLights() {
        FrameLighting lighting;

        for (int i = 0; i < 6; ++i) {
            lighting.spotLights.push_back(spotLightAt(Vec3{static_cast<float>(i), 3.0f, 0.0f}, true, 50.0f));
        }

        for (int i = 0; i < 40; ++i) {
            lighting.pointLights.push_back(pointLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, true, 40.0f));
        }

        // In the usual atlas (High's), which these lights overflow.
        ShadowSettings high;
        high.cascadeResolution = CascadeResolution::Pixels2048;
        const ShadowPlan plan = planShadows(lighting, makeOverviewCamera(), high);
        assertTilesFitTheAtlas(plan, shadowAtlasSizeFor(high));

        for (int slot = 0; slot < 40; ++slot) {
            const std::size_t first = static_cast<std::size_t>(plan.points[static_cast<std::size_t>(slot)].firstTile);
            assert(plan.tiles[first].size == smallestShadowTileSize);
        }

        std::uint32_t previousSize = largestShadowTileSize;
        for (int slot = 0; slot < 6; ++slot) {
            const std::uint32_t size = plan.tiles[static_cast<std::size_t>(plan.spots[static_cast<std::size_t>(slot)].firstTile)].size;
            assert(size <= previousSize);
            previousSize = size;
        }
        assert(plan.tiles[static_cast<std::size_t>(plan.spots[0].firstTile)].size == largestShadowTileSize);
        assert(previousSize < largestShadowTileSize);
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

        const ShadowPlan plan = planShadows(lighting, makeOverviewCamera());

        assert(plan.directional[0].firstTile == 0);
        // The sun's cascades take the first tiles.
        const int spotTile = maxShadowCascades;
        assert(plan.spots.size() == 1 && plan.spots[0].firstTile == spotTile);
        assert(plan.points.size() == 3);
        assert(plan.points[0].firstTile == spotTile + 1);
        assert(plan.points[1].firstTile == -1);
        assert(plan.points[2].firstTile == spotTile + 7);
        assert(plan.tiles.size() == static_cast<std::size_t>(spotTile + 13));
        assertTilesFitTheAtlas(plan);

        // The shadow pass draws with the face's world matrix. The shader gets the same view of the light placed relative
        // to the camera (the shader's origin), column-major.
        const std::size_t secondFaceTile = static_cast<std::size_t>(spotTile) + 2;
        const Vec3 light{1.0f, 0.0f, 0.0f};
        const Mat4 secondFace = pointShadowFaceMatrix(light, 1, 20.0f, plan.tiles[secondFaceTile].size);
        const Mat4 secondFaceForShader = pointShadowFaceMatrix(light - makeOverviewCamera().getPosition(), 1, 20.0f, plan.tiles[secondFaceTile].size);

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                assert(nearlyEqual(plan.tiles[secondFaceTile].matrix.values[row][column], secondFace.values[row][column]));
                assert(nearlyEqual(plan.tileData[secondFaceTile].matrix[column * 4 + row], secondFaceForShader.values[row][column]));
            }
        }

        // Perspective tiles offset by distance from the light; the cascade boxes by a fixed amount.
        assert(plan.tileData[secondFaceTile].offset[0] > 0.0f);
        assert(plan.tileData[static_cast<std::size_t>(spotTile)].offset[0] > 0.0f);
        assert(plan.tileData[0].offset[0] == 0.0f && plan.tileData[0].offset[1] > 0.0f);

        // A sun's cascades get the largest tiles while there is room.
        assert(plan.tiles[0].size == cascadeTilePixels(ShadowSettings{}.cascadeResolution));
    }

    // The 64 most important point lights can have a shadow; lights past that still shine, unshadowed.
    void testShadowedLightsAreCapped() {
        FrameLighting lighting;

        for (int i = 0; i < 70; ++i) {
            lighting.pointLights.push_back(pointLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, true));
        }

        const ShadowPlan plan = planShadows(lighting, makeOverviewCamera());

        assert(maxShadowedPointLights == 64 && maxShadowedSpotLights == 8);
        assert(plan.tiles.size() == 64 * 6);
        assert(plan.points[0].firstTile == 0);
        assert(plan.points[63].firstTile == 63 * 6);

        // One shadow record per light the shader gets, so the lights past the shadow cap say "none".
        assert(plan.points.size() == 70);

        for (std::size_t slot = 64; slot < 70; ++slot) {
            assert(plan.points[slot].firstTile == -1);
        }
        assertTilesFitTheAtlas(plan);
    }

    // When the lights ask for more than the atlas holds, the least important (latest slots) shrink first, and a point
    // light's six faces always share one size.
    void testTilesShrinkToFit() {
        FrameLighting lighting;

        for (int i = 0; i < 4; ++i) {
            lighting.directionalLights.push_back(DirectionalLight{});
        }

        // Spotlights reaching 20 ask for 1024 tiles, so the point lights are the ones that must shrink.
        for (int i = 0; i < 8; ++i) {
            lighting.spotLights.push_back(spotLightAt(Vec3{static_cast<float>(i), 3.0f, 0.0f}, true, 20.0f));
        }

        for (int i = 0; i < 16; ++i) {
            lighting.pointLights.push_back(pointLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, true, 40.0f));
        }

        const ShadowPlan plan = planShadows(lighting, makeOverviewCamera());

        assert(plan.tiles.size() == 4 + 8 + 96);
        assertTilesFitTheAtlas(plan);

        // The brightest sun's cascades keep their size while point lights can still shrink; the other suns get no shadow.
        for (int cascade = 0; cascade < maxShadowCascades; ++cascade) {
            assert(plan.tiles[static_cast<std::size_t>(plan.directional[0].firstTile + cascade)].size == cascadeTilePixels(ShadowSettings{}.cascadeResolution));
        }

        std::uint32_t previousSize = standardShadowTileSize;

        for (int slot = 0; slot < 16; ++slot) {
            const std::size_t first = static_cast<std::size_t>(plan.points[static_cast<std::size_t>(slot)].firstTile);
            const std::uint32_t size = plan.tiles[first].size;

            for (std::size_t face = 1; face < 6; ++face) {
                assert(plan.tiles[first + face].size == size);
            }

            assert(size <= previousSize);
            previousSize = size;
        }

        // The most important point light keeps more of the size it asked for than the least important.
        assert(plan.tiles[static_cast<std::size_t>(plan.points[0].firstTile)].size > previousSize);
        assert(previousSize < standardShadowTileSize);
    }

    // The worst case: every shadowed light there can be, all asking for the largest tiles. Everything still fits, with
    // every point light's faces down to the smallest and, as the last resort, the sun's cascades down to the standard size.
    // Every shadowed light there can be, all asking for the largest tiles.
    FrameLighting everyShadowedLight() {
        FrameLighting lighting;

        for (int i = 0; i < maxShadowedDirectionalLights; ++i) {
            lighting.directionalLights.push_back(DirectionalLight{});
        }

        for (int i = 0; i < maxShadowedSpotLights; ++i) {
            lighting.spotLights.push_back(spotLightAt(Vec3{static_cast<float>(i), 3.0f, 0.0f}, true, 50.0f));
        }

        for (int i = 0; i < maxShadowedPointLights; ++i) {
            lighting.pointLights.push_back(pointLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, true, 40.0f));
        }

        return lighting;
    }

    void testEveryShadowFits() {
        // In the usual atlas (High's 2048 cascades), where the cascades must give way last.
        ShadowSettings high;
        high.cascadeResolution = CascadeResolution::Pixels2048;
        const ShadowPlan plan = planShadows(everyShadowedLight(), makeOverviewCamera(), high);

        assert(plan.tiles.size() == static_cast<std::size_t>(maxShadowTiles));
        assertTilesFitTheAtlas(plan, shadowAtlasSizeFor(high));
        assert(plan.tiles[static_cast<std::size_t>(plan.directional[0].firstTile)].size == standardShadowTileSize);
        assert(plan.tiles[static_cast<std::size_t>(plan.points[0].firstTile)].size == smallestShadowTileSize);
    }

    /*
    * The atlas holds the sun's cascades at up to 2048 pixels in its usual 8192 x 4096 (128 MB of depth). At 4096 (the
    * default, the Ultra preset) they need four times the room, so it grows by exactly their 4 x 4096^2 texels:
    * 8192 x 12288, 384 MB, with squares of up to 4096.
    */
    void testAtlasGrowsForUltraCascades() {
        ShadowSettings settings;
        assert(settings.cascadeResolution == CascadeResolution::Pixels4096);
        assert(cascadeTilePixels(CascadeResolution::Pixels1024) == 1024);
        assert(cascadeTilePixels(CascadeResolution::Pixels2048) == 2048);
        assert(cascadeTilePixels(CascadeResolution::Pixels4096) == 4096);
        assert(shadowAtlasSizeFor(settings) == (ShadowAtlasSize{8192, 12288, 4096}));

        const ShadowAtlasSize standard{8192, 4096, 2048};
        assert(ShadowAtlasSize{} == standard);

        settings.cascadeResolution = CascadeResolution::Pixels2048;
        assert(shadowAtlasSizeFor(settings) == standard);

        settings.cascadeResolution = CascadeResolution::Pixels1024;
        assert(shadowAtlasSizeFor(settings) == standard);
    }

    // The cascades get the tiles the setting asks for: 1024 on Low, 4096 on Ultra in the grown atlas.
    void testCascadesFollowTheirResolution() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.4f, -1.0f, -0.6f}});

        for (CascadeResolution resolution : {CascadeResolution::Pixels1024, CascadeResolution::Pixels4096}) {
            ShadowSettings settings;
            settings.cascadeResolution = resolution;
            ShadowAtlasLayout layout{shadowAtlasSizeFor(settings)};
            const ShadowPlan plan = planShadows(lighting, makeTestCamera(Vec3{2.0f, 5.0f, 4.0f}), layout, settings);

            assertTilesFitTheAtlas(plan, layout.size());

            for (const ShadowTile& tile : plan.tiles) {
                assert(tile.size == cascadeTilePixels(resolution));
            }
        }
    }

    // On Ultra the worst case fits too, and the grown atlas leaves the cascades their full 4096.
    void testEveryShadowFitsOnUltra() {
        ShadowSettings settings;
        settings.cascadeResolution = CascadeResolution::Pixels4096;
        ShadowAtlasLayout layout{shadowAtlasSizeFor(settings)};
        const ShadowPlan plan = planShadows(everyShadowedLight(), makeOverviewCamera(), layout, settings);

        assert(plan.tiles.size() == static_cast<std::size_t>(maxShadowTiles));
        assertTilesFitTheAtlas(plan, layout.size());

        for (int cascade = 0; cascade < maxShadowCascades; ++cascade) {
            assert(plan.tiles[static_cast<std::size_t>(plan.directional[0].firstTile + cascade)].size == 4096);
        }
    }

    // A layout for another atlas size than the settings ask for is the caller's mistake (the renderer remakes both).
    void testLayoutMustMatchTheSettings() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{});
        ShadowSettings ultra;
        ultra.cascadeResolution = CascadeResolution::Pixels4096;
        ShadowAtlasLayout standard;
        bool threw = false;

        try {
            planShadows(lighting, makeTestCamera(Vec3{2.0f, 5.0f, 4.0f}), standard, ultra);
        }
        catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
    }

    // Small lights get small tiles, so sixteen lamps take a small corner of the atlas.
    void testSmallLightsGetSmallTiles() {
        FrameLighting lighting;

        for (int i = 0; i < 16; ++i) {
            lighting.pointLights.push_back(pointLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, true, 5.0f));
        }

        const ShadowPlan plan = planShadows(lighting, makeOverviewCamera());

        assert(plan.tiles.size() == 96);
        assertTilesFitTheAtlas(plan);

        for (const ShadowTile& tile : plan.tiles) {
            assert(tile.size == smallestShadowTileSize);
        }
    }

    // With no lights there is nothing to draw and no slot to describe.
    void testNoShadowsWithoutLights() {
        const ShadowPlan plan = planShadows(FrameLighting{}, makeOverviewCamera());

        assert(plan.tiles.empty());
        assert(plan.directional.empty() && plan.spots.empty() && plan.points.empty());
    }

    // A slot's tiles are found in its own record however far down the list it is.
    void testShadowsAreFoundForAnySlot() {
        FrameLighting lighting;

        for (int i = 0; i < 20; ++i) {
            lighting.pointLights.push_back(pointLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, i == 19));
        }

        for (int i = 0; i < 8; ++i) {
            lighting.spotLights.push_back(spotLightAt(Vec3{static_cast<float>(i), 3.0f, 0.0f}, i != 0));
        }

        const ShadowPlan plan = planShadows(lighting, makeOverviewCamera());

        // Spot slot 0 casts none; slots 1 to 7 take one tile each.
        assert(plan.spots[0].firstTile == -1);
        assert(plan.spots[1].firstTile == 0);
        assert(plan.spots[7].firstTile == 6);

        assert(plan.points[19].firstTile == 7);
        assert(plan.points[18].firstTile == -1);
        assert(plan.tiles.size() == 13);
    }
}

namespace {
    Camera makeForwardCamera() {
        return Camera{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.5f, 0.1f, 100.0f};
    }

    // A lamp in the middle of the view: every face sees part of it, so all six get tiles.
    void testFacesInViewGetTiles() {
        FrameLighting lighting;
        lighting.pointLights.push_back(pointLightAt(Vec3{0.0f, 0.0f, -10.0f}, true, 8.0f));

        const ShadowPlan plan = planShadows(lighting, makeForwardCamera());

        assert(plan.tiles.size() == 6);

        for (const ShadowTile& tile : plan.tiles) {
            assert(tile.size > 0);
        }
    }

    // A lamp behind the camera whose light reaches forwards into view: only the face looking forwards (-Z) can shadow
    // anything on screen. The other five get no room in the atlas and nothing is drawn into them, but keep their places
    // in the tile list, so the shader still finds a face at its first tile plus the face number.
    void testFacesOutOfViewAreLeftOut() {
        FrameLighting lighting;
        lighting.pointLights.push_back(pointLightAt(Vec3{0.0f, 0.0f, 5.0f}, true, 8.0f));

        const ShadowPlan plan = planShadows(lighting, makeForwardCamera());

        assert(plan.points[0].firstTile == 0);
        assert(plan.tiles.size() == 6);
        assertTilesFitTheAtlas(plan);

        for (int face = 0; face < 5; ++face) {
            assert(plan.tiles[static_cast<std::size_t>(face)].size == 0);
        }

        assert(plan.tiles[5].size > 0);

        // The shader takes the light's normal offset from its first tile, so a left-out face still carries it.
        assert(plan.tileData[0].offset[0] > 0.0f);
        assert(plan.tileData[0].offset[0] == plan.tileData[5].offset[0]);
    }

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

        const ShadowPlan plan = planShadows(lighting, makeOverviewCamera());

        assert(nearlyEqual(plan.points[0].strength, 0.3f));
        assert(nearlyEqual(plan.points[1].strength, 0.0f));
        assert(nearlyEqual(plan.spots[0].strength, 0.7f));
    }

    // The shader reads each light's shadow from the light's own record: its first tile, how strong the shadow is while it
    // fades, and the glowing size that softens it. Lights the plan gave no tiles keep "none".
    void testShadowsAreAttachedToLights() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{});

        PlacedPointLight shadowed = pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, true);
        shadowed.shadowFade = 0.3f;
        shadowed.light.emitterRadius = 0.2f;
        lighting.pointLights.push_back(shadowed);
        lighting.pointLights.push_back(pointLightAt(Vec3{1.0f, 0.0f, 0.0f}, false));

        PlacedSpotLight spot = spotLightAt(Vec3{0.0f, 3.0f, 0.0f}, true);
        spot.shadowFade = 0.7f;
        spot.light.emitterRadius = 0.03f;
        lighting.spotLights.push_back(spot);

        const Camera camera = makeOverviewCamera();
        const ShadowPlan plan = planShadows(lighting, camera);
        GpuLights lights = packLighting(lighting, camera.getPosition());
        attachShadows(lights, plan);

        // The sun's cascades come first, then the spotlight, then the point light's faces.
        assert(lights.directional[0].shadowTile == 0 && lights.directional[0].cascadeCount == maxShadowCascades);
        assert(lights.spots[0].shadowTile == maxShadowCascades);
        assert(nearlyEqual(lights.spots[0].shadowStrength, 0.7f) && nearlyEqual(lights.spots[0].emitterRadius, 0.03f));
        assert(lights.points[0].firstShadowTile == maxShadowCascades + 1);
        assert(nearlyEqual(lights.points[0].shadowStrength, 0.3f) && nearlyEqual(lights.points[0].emitterRadius, 0.2f));
        assert(lights.points[1].firstShadowTile == -1 && lights.points[1].shadowStrength == 0.0f);
    }

    // Each tile knows how far its light reaches (a sphere round the light, the range as radius), so the shadow pass can
    // drop casters that light never reaches with one sphere test before the face's frustum. A directional light's box
    // has no such limit (negative radius), nor do a sun's cascade boxes.
    void testTilesKnowTheirLightsReach() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{});
        lighting.spotLights.push_back(spotLightAt(Vec3{0.0f, 3.0f, 0.0f}, true, 10.0f));
        lighting.pointLights.push_back(pointLightAt(Vec3{1.0f, 0.0f, 0.0f}, true, 20.0f));

        const ShadowPlan plan = planShadows(lighting, makeOverviewCamera());

        const std::size_t spotTile = maxShadowCascades;
        assert(plan.tiles.size() == spotTile + 7);

        for (std::size_t cascade = 0; cascade < spotTile; ++cascade) {
            assert(plan.tiles[cascade].reach.radius < 0.0f);
        }

        assert(nearlyEqual(plan.tiles[spotTile].reach.radius, 10.0f) && nearlyEqual(plan.tiles[spotTile].reach.centre.y, 3.0f));

        for (std::size_t face = spotTile + 1; face < spotTile + 7; ++face) {
            assert(nearlyEqual(plan.tiles[face].reach.radius, 20.0f));
            assert(nearlyEqual(plan.tiles[face].reach.centre.x, 1.0f) && nearlyEqual(plan.tiles[face].reach.centre.y, 0.0f));
        }
    }

    // A tile's shader matrix, from the column-major numbers the shader reads.
    Mat4 shaderMatrix(const ShadowTileData& data) {
        Mat4 matrix;

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                matrix.values[row][column] = data.matrix[column * 4 + row];
            }
        }

        return matrix;
    }

    /*
    * The shadow pass draws in world space (so a still light's tiles stay cached however the camera moves), while the
    * shader works relative to the camera. Both matrices must put every point on the same texel at the same depth: the
    * shader's matrix, given the point measured from the camera, lands where the pass's matrix put the world point.
    */
    void testShaderMatricesAreCameraRelative() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.3f, -1.0f, -0.4f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f, true});
        lighting.spotLights.push_back(spotLightAt(Vec3{6.0f, 4.0f, 9.0f}, true));
        lighting.pointLights.push_back(pointLightAt(Vec3{5.0f, 1.5f, 8.0f}, true));

        const Camera camera = makeTestCamera(Vec3{7.0f, 3.0f, 12.0f});
        const ShadowPlan plan = planShadows(lighting, camera);
        const Vec3 points[4] = {Vec3{6.0f, 0.0f, 7.0f}, Vec3{4.0f, 0.5f, 9.5f}, Vec3{7.5f, 1.0f, 6.0f}, Vec3{5.0f, 0.2f, 3.0f}};
        int compared = 0;
        int tilesCompared = 0;

        for (std::size_t i = 0; i < plan.tiles.size(); ++i) {
            if (plan.tiles[i].size == 0) {
                continue;
            }

            const int comparedBefore = compared;

            const Mat4 forShader = shaderMatrix(plan.tileData[i]);

            for (const Vec3& point : points) {
                const Vec3 drawn = project(plan.tiles[i].matrix, point);

                // Only points the tile covers are ever looked up in it (behind a cube face, w is near zero or negative).
                if (!landsInside(drawn)) {
                    continue;
                }

                const Vec3 lookedUp = project(forShader, point - camera.getPosition());

                assert(nearlyEqual(lookedUp.x, drawn.x, 1e-4f) && nearlyEqual(lookedUp.y, drawn.y, 1e-4f));
                assert(nearlyEqual(lookedUp.z, drawn.z, 1e-5f));
                ++compared;
            }

            tilesCompared += compared > comparedBefore ? 1 : 0;
        }

        // The directional box, the spotlight and at least one cube face were all checked.
        assert(tilesCompared >= 3);
    }
}

void testShadowMap() {
    testAtlasGrowsForUltraCascades();
    testCascadesFollowTheirResolution();
    testEveryShadowFitsOnUltra();
    testLayoutMustMatchTheSettings();
    testFacesInViewGetTiles();
    testFacesOutOfViewAreLeftOut();
    testShadowStrengthIsPassed();
    testShadowsAreAttachedToLights();
    testTilesKnowTheirLightsReach();
    testShaderMatricesAreCameraRelative();
    testFaceFollowsTheLargestAxis();
    testEachFaceLooksAlongItsAxis();
    testEveryDirectionLandsInsideItsFace();
    testDepthShrinksWithDistance();
    testOrthographic();
    testSpotShadowCoversTheCone();
    testSunGetsOneTilePerCascade();
    testNearShadowsAreFine();
    testCascadesCoverTheViewToTheShadowDistance();
    testFittedCascadesEndAtTheFittedDistance();
    testOnlyTheFirstDirectionalLightCastsShadows();
    testCascadesMoveInWholeTexels();
    testLightsCastShadowsByDefault();
    testTileSizeFollowsTheLightsReach();
    testSpotlightsGetFinerTiles();
    testSpotTilesShrinkAfterPointLights();
    testTilesAreHandedOut();
    testShadowedLightsAreCapped();
    testTilesShrinkToFit();
    testEveryShadowFits();
    testSmallLightsGetSmallTiles();
    testNoShadowsWithoutLights();
    testShadowsAreFoundForAnySlot();
}
