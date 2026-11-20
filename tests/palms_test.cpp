#include "games/forest/palms.h"
#include "games/forest/forest_ground.h"
#include "games/forest/forest_game.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/understorey.h"
#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "math/angles.h"
#include "scene/model_renderer.h"

#include "forest_helpers.h"
#include "test_levels.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <vector>

namespace {
    // A segment's angle above level (radians).
    float elevationOf(const Vec3& from, const Vec3& to) {
        const Vec3 step = to - from;
        return std::atan2(step.y, std::sqrt(step.x * step.x + step.z * step.z));
    }

    // The mean brightness of a picture's texels in a box of u and v (0..1).
    float meanBrightness(const ImageData& image, float uFrom, float uTo, float vFrom, float vTo) {
        float sum = 0.0f;
        int count = 0;

        for (std::uint32_t y = 0; y < image.height; ++y) {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(image.height);

            for (std::uint32_t x = 0; x < image.width; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(image.width);

                if (u >= uFrom && u <= uTo && v >= vFrom && v <= vTo) {
                    const std::size_t texel = (static_cast<std::size_t>(y) * image.width + x) * 4;
                    sum += static_cast<float>(image.pixels[texel]) + image.pixels[texel + 1] + image.pixels[texel + 2];
                    ++count;
                }
            }
        }

        return count > 0 ? sum / static_cast<float>(count) : 0.0f;
    }

    /*
    * A frond's bend (docs/research/JUNGLE.md, Palms): a tapering cantilever under its own weight, so its turn grows from 0 at the
    * crown to all of it at the tip, never turning back, and bends most in its middle (the moment falls towards the tip,
    * the stiffness faster still near it, then the moment wins).
    */
    void testAFrondBendsLikeATaperingCantilever() {
        assert(near(frondBendShare(0.0f), 0.0f, 1e-6f) && near(frondBendShare(1.0f), 1.0f, 1e-5f));
        float last = 0.0f;

        for (int step = 1; step <= 100; ++step) {
            const float share = frondBendShare(static_cast<float>(step) / 100.0f);
            assert(share >= last);
            last = share;
        }

        const float middle = frondBendShare(0.55f) - frondBendShare(0.45f);
        assert(middle > frondBendShare(0.1f) - frondBendShare(0.0f));
        assert(middle > frondBendShare(1.0f) - frondBendShare(0.9f));

        // Without the taper the curvature would fall all the way to the tip: half the turn would come in the first fifth.
        assert(frondBendShare(0.2f) < 0.5f);
    }

    // The leaflets' outlines: a spindle along the frond (longest in the middle, a third of it at the ends), each leaflet
    // narrow at its base, widest before its middle and pointed.
    void testTheLeafletsOutlines() {
        assert(near(palmLeafletLength(0.5f), 1.0f, 1e-5f));
        assert(near(palmLeafletLength(0.0f), 1.0f / 3.0f, 0.02f) && near(palmLeafletLength(1.0f), 1.0f / 3.0f, 0.02f));
        assert(palmLeafletLength(0.25f) < palmLeafletLength(0.5f) && palmLeafletLength(0.25f) > palmLeafletLength(0.0f));

        assert(near(palmLeafletWidth(1.0f), 0.0f, 1e-5f) && palmLeafletWidth(0.0f) < 0.3f);
        float widest = 0.0f;
        float widestAt = 0.0f;

        for (int step = 0; step <= 100; ++step) {
            const float along = static_cast<float>(step) / 100.0f;

            if (palmLeafletWidth(along) > widest) {
                widest = palmLeafletWidth(along);
                widestAt = along;
            }
        }

        assert(near(widest, 1.0f, 0.01f) && widestAt > 0.2f && widestAt < 0.5f);
    }

    /*
    * A frond's rachis: its points from the base, as long as asked, leaving at its elevation and ending turned down by
    * its droop, all in the upright plane of its azimuth (turned from +x towards -z).
    */
    void testAFrondsSpine() {
        const Vec3 base{1.0f, 2.0f, 3.0f};
        const float elevation = radiansFromDegrees(60.0f);
        const float droop = radiansFromDegrees(90.0f);
        const int count = 40;
        const std::vector<Vec3> spine = frondSpine(base, 0.0f, elevation, 2.0f, droop, count);
        assert(spine.size() == static_cast<std::size_t>(count + 1) && (spine.front() - base).lengthSquared() == 0.0f);
        float length = 0.0f;

        for (std::size_t point = 1; point < spine.size(); ++point) {
            length += (spine[point] - spine[point - 1]).length();
            assert(near(spine[point].z, base.z, 1e-5f));
        }

        assert(near(length, 2.0f, 1e-3f));
        const float step = droop / static_cast<float>(count);
        assert(std::fabs(elevationOf(spine[0], spine[1]) - elevation) < step);
        assert(std::fabs(elevationOf(spine[count - 1], spine[count]) - (elevation - droop)) < step);

        // Turned a quarter round, the same frond leaves towards -z.
        const std::vector<Vec3> turned = frondSpine(base, 0.5f * 3.14159265f, elevation, 2.0f, droop, count);
        assert(near(turned.back().x, base.x, 1e-4f) && turned[count / 2].z < base.z);
    }

    /*
    * The variants: heights evenly spread between the shortest and the tallest, taller ones with more and longer fronds,
    * each frond a share of its palm's height and its stem what is left under its crown.
    */
    void testThePalmVariants() {
        const ForestPalmSettings settings;
        float lastHeight = 0.0f;
        int lastFronds = 0;

        for (int variant = 0; variant < settings.variants; ++variant) {
            const float height = palmVariantHeight(settings, variant);
            assert(height > lastHeight && height > settings.shortest && height < settings.tallest);
            const PalmShape shape = palmVariantShape(settings, variant, 77);
            assert(shape.height == height);
            assert(shape.fronds >= settings.leastFronds && shape.fronds <= settings.mostFronds && shape.fronds >= lastFronds);
            assert(near(shape.frondLength, palmFrondLengthShare * height, 1e-5f));
            assert(shape.stemHeight >= 0.0f && shape.stemHeight < height && shape.stemRadius > 0.0f);
            assert(shape.leafletPairs == settings.leafletPairs && shape.leafletWidthShare == settings.leafletWidthShare);
            lastHeight = height;
            lastFronds = shape.fronds;
        }

        assert(near(palmVariantHeight(settings, 0) - settings.shortest, settings.tallest - palmVariantHeight(settings, settings.variants - 1), 1e-4f));
    }

    // Every vertex's normal unit length, its tangent square to it, its texture coordinates in the picture.
    void assertWellFormed(const IndexedMesh& mesh) {
        assert(!mesh.indices.empty() && mesh.indices.size() % 3 == 0);
        assert(trianglesFaceTheirNormals(mesh));

        for (const MeshVertex& vertex : mesh.vertices) {
            assert(near(vertex.normal.length(), 1.0f, 1e-3f));
            const Vec3 tangent{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z};
            assert(near(tangent.length(), 1.0f, 1e-3f) && std::fabs(tangent.dot(vertex.normal)) < 1e-3f);
            assert(vertex.tangent.w == 1.0f || vertex.tangent.w == -1.0f);
            assert(vertex.uv.x >= 0.0f && vertex.uv.x <= 1.0f);
        }
    }

    /*
    * A palm grown: its top at its height, its stem's foot just under the ground, its fronds spreading round it; well
    * formed; the coarser level and the shadow's stand-in fewer triangles each, with growing errors; the same every time.
    */
    void testAPalmIsGrown() {
        const ForestPalmSettings settings;
        const PalmShape shape = palmVariantShape(settings, settings.variants / 2, 5);
        const PalmMeshes meshes = growPalmMeshes(shape);

        for (const IndexedMesh* mesh : {&meshes.palm, &meshes.level, &meshes.shadow}) {
            assertWellFormed(*mesh);
            const Aabb bounds = indexedMeshBounds(*mesh);
            assert(near(bounds.maximum.y, shape.height, 0.02f * shape.height));
            assert(bounds.minimum.y < 0.0f && bounds.minimum.y > -0.2f);
            assert(bounds.maximum.x > 0.3f * shape.frondLength && bounds.minimum.x < -0.3f * shape.frondLength);
            assert(bounds.maximum.z > 0.3f * shape.frondLength && bounds.minimum.z < -0.3f * shape.frondLength);
        }

        assert(near(indexedMeshBounds(meshes.palm).maximum.y, shape.height, 1e-3f));

        // Every leaflet's two halves on every frond, each half two triangles a segment but one in the last, to its point.
        const std::size_t leafletTriangles = static_cast<std::size_t>(shape.fronds * shape.leafletPairs * 2 * 2 * (2 * PalmDetail{}.leafletSegments - 1));
        assert(meshes.palm.indices.size() / 3 >= leafletTriangles);
        assert(meshes.level.indices.size() < meshes.palm.indices.size() && meshes.shadow.indices.size() < meshes.palm.indices.size());
        assert(meshes.levelError > 0.0f && meshes.shadowError > 0.0f);

        const PalmMeshes again = growPalmMeshes(shape);
        assert(again.palm.vertices.size() == meshes.palm.vertices.size() && (again.palm.vertices.back().position - meshes.palm.vertices.back().position).lengthSquared() == 0.0f);
    }

    /*
    * A palm thins leaflet by leaflet (scene/card_thinning.h, docs/research/LOD.md, Hiding the switches): grown thinned it holds
    * exactly the full palm's vertices, only reordered, every palmKeepEvery-th pair's leaflets first with the stem and the
    * stalks, so the kept leaflets stand where they stood; the rest after, each ranked past the least share. The stem and
    * stalks are their own anchors, so they never move; each leaflet's vertices share its base. The coarser level is the
    * kept part alone (thinnedLevel), its error the gap the dropped pairs leave, the fade's at the least share.
    */
    void testAPalmThinsLeafletByLeaflet() {
        const ForestPalmSettings settings;
        const PalmShape shape = palmVariantShape(settings, settings.variants / 2, 5);
        const IndexedMesh whole = growPalm(shape, PalmDetail{});
        const IndexedMesh thinned = growPalm(shape, PalmDetail{.thinned = true});
        assert(whole.thinningCards.empty() && thinned.vertices.size() == whole.vertices.size() && thinned.indices.size() == whole.indices.size());

        const auto sortedPositions = [](const IndexedMesh& mesh) {
            std::vector<std::array<float, 3>> positions;

            for (const MeshVertex& vertex : mesh.vertices) {
                positions.push_back({vertex.position.x, vertex.position.y, vertex.position.z});
            }

            std::sort(positions.begin(), positions.end());
            return positions;
        };
        assert(sortedPositions(thinned) == sortedPositions(whole));

        const PalmMeshes meshes = growPalmMeshes(shape);
        const CardThinning& thinning = meshes.palm.thinning;
        assert(thinning.leastShare == 1.0f / static_cast<float>(palmKeepEvery) && thinning.growth == shape.leafletGrowth && thinning.cardSize > 0.0f);
        assert(meshes.palm.thinningCards.size() == meshes.palm.vertices.size());
        assert(near(meshes.levelError, thinningError(thinning, thinning.leastShare), 1e-6f));

        const IndexedMesh level = thinnedLevel(meshes.palm);
        assert(level.vertices.size() == meshes.level.vertices.size() && level.indices == meshes.level.indices);
        assert(level.vertices.size() < meshes.palm.vertices.size() && level.vertices.size() > meshes.palm.vertices.size() / (2 * palmKeepEvery));
        std::size_t stillVertices = 0;

        for (std::size_t vertex = 0; vertex < meshes.palm.vertices.size(); ++vertex) {
            const ThinningCard& card = meshes.palm.thinningCards[vertex];
            const Vec3 fromAnchor = meshes.palm.vertices[vertex].position - card.anchor;
            stillVertices += fromAnchor.lengthSquared() == 0.0f ? 1 : 0;

            if (vertex < level.vertices.size()) {
                assert(card.rank == 0.0f);
            } else {
                assert(card.rank >= thinning.leastShare + cardThinningBand - 1e-6f && card.rank <= 1.0f);
            }
        }

        // The stem and the stalks: rings of vertices a frond, all their own anchors.
        assert(stillVertices >= static_cast<std::size_t>(shape.fronds) * 8);
    }

    /*
    * The understorey stands on its layer's dithered cells (valley_maps), in order: each plant on a cell's place, the
    * few cells left out only by the trunk or the maps.
    */
    void testTheUnderstoreyStandsOnItsCells() {
        const ForestUnderstoreySettings settings;
        const ValleyMaps& maps = patchValleyMaps();
        const std::vector<StandingCrown> trunks{StandingCrown{Vec2{valleyAxis(-100.0f) + 15.0f, -100.0f}, 2.0f}};
        const std::vector<UnderstoreyPlacement> plants = placeUnderstorey(settings, maps, trunks);
        const std::vector<DitheredCell> cells = ditheredCells(settings.planting, maps, forestPatch, understoreySeed);
        assert(!plants.empty() && plants.size() <= cells.size() && 10 * plants.size() > 9 * cells.size());
        std::size_t next = 0;

        for (const UnderstoreyPlacement& plant : plants) {
            while (next < cells.size() && (cells[next].place.x != plant.foot.x || cells[next].place.y != plant.foot.z)) {
                ++next;
            }

            assert(next < cells.size());
            ++next;
        }
    }

    // The palms' places: on the trees' stretch, clear of the path and the trunks, on the ground, every variant used,
    // none blocking a sight line; the same every run.
    void testThePalmsArePlaced() {
        const ForestPalmSettings settings;
        const std::vector<StandingCrown> trunks{StandingCrown{Vec2{valleyAxis(-150.0f) + 10.0f, -150.0f}, 2.0f}};
        const std::vector<SightLine> lines = forestSightLines();
        const std::vector<PalmPlacement> palms = placePalms(settings, patchValleyMaps(), trunks);
        assert(palms.size() > 100);
        std::vector<int> variants(static_cast<std::size_t>(settings.variants), 0);

        for (const PalmPlacement& palm : palms) {
            assert(palm.foot.z >= treeNorthmost && palm.foot.z <= treeSouthmost);
            assert(std::fabs(palm.foot.x - valleyAxis(palm.foot.z)) <= treeSpread);
            assert(forestPathDistance(palm.foot.x, palm.foot.z) >= settings.planting.pathClearance);
            assert(near(palm.foot.y, forestHeight(palm.foot.x, palm.foot.z) - understoreySinkMetres, 1e-4f));
            assert((Vec2{palm.foot.x, palm.foot.z} - trunks.front().foot).length() >= trunks.front().radius + settings.trunkClearance);
            assert(palm.scale >= 1.0f - palmScaleSpread - 1e-5f && palm.scale <= 1.0f + palmScaleSpread + 1e-5f);
            assert(palm.variant >= 0 && palm.variant < settings.variants);
            assert(!blocksSight(palmCentre(settings, palm), palmRadius(settings, palm), lines));
            ++variants[static_cast<std::size_t>(palm.variant)];
        }

        for (int count : variants) {
            assert(count > 0);
        }

        const PalmPlacement& first = palms.front();
        assert(near(palmRadius(settings, first), 0.5f * palmVariantHeight(settings, first.variant) * first.scale, 1e-5f));
        assert(near(palmCentre(settings, first).y, first.foot.y + palmRadius(settings, first), 1e-4f));

        const std::vector<PalmPlacement> again = placePalms(settings, patchValleyMaps(), trunks);
        assert(again.size() == palms.size() && again.back().foot.x == palms.back().foot.x && again.back().variant == palms.back().variant);
    }

    /*
    * The leaflet's pictures: opaque everywhere (the outline is geometry), a paler midrib down the middle, a browner dry
    * tip, the midrib the highest ridge across; the same every time.
    */
    void testTheLeafletsPictures() {
        const Pixel green{80, 120, 60};
        const PalmLeafletImages images = palmLeafletImages(128, green, 9);
        assert(images.colour.width == 128 && images.colour.height == 128 && images.colour.pixels.size() == 128u * 128u * 4u);
        assert(images.colour.encoding == TextureEncoding::Srgb);
        assert(images.height.width == 128 && images.height.height == 128 && images.height.pixels.size() == 128u * 128u * 4u);

        for (std::size_t texel = 0; texel < images.colour.pixels.size() / 4; ++texel) {
            assert(images.colour.pixels[texel * 4 + 3] == 255);
        }

        // Paler on the midrib than between it and the edge.
        assert(meanBrightness(images.colour, 0.49f, 0.51f, 0.2f, 0.8f) > 1.1f * meanBrightness(images.colour, 0.2f, 0.3f, 0.2f, 0.8f));

        // Redder against green at the tip than in the middle.
        const auto redShare = [&](float vFrom, float vTo) {
            float red = 0.0f;
            float greenSum = 0.0f;

            for (std::uint32_t y = 0; y < 128; ++y) {
                const float v = (static_cast<float>(y) + 0.5f) / 128.0f;

                if (v >= vFrom && v <= vTo) {
                    for (std::uint32_t x = 16; x < 48; ++x) {
                        red += images.colour.pixels[(y * 128 + x) * 4];
                        greenSum += images.colour.pixels[(y * 128 + x) * 4 + 1];
                    }
                }
            }

            return red / greenSum;
        };
        assert(redShare(0.97f, 1.0f) > redShare(0.4f, 0.6f) + 0.05f);

        // Across the middle row the midrib is the highest.
        const std::uint32_t row = 64;
        std::uint8_t highest = 0;
        std::uint32_t highestAt = 0;

        for (std::uint32_t x = 0; x < 128; ++x) {
            const std::uint8_t height = images.height.pixels[(row * 128 + x) * 4];

            if (height > highest) {
                highest = height;
                highestAt = x;
            }
        }

        assert(highestAt >= 62 && highestAt <= 65);

        const PalmLeafletImages again = palmLeafletImages(128, green, 9);
        assert(again.colour.pixels == images.colour.pixels && again.height.pixels == images.height.pixels);
    }

    // A leaf atlas's mean colour counts its leaves, not the gaps between them; nothing in it gives the fallback.
    void testAMeanColourCountsOnlyTheLeaves() {
        ImageData image;
        image.width = 2;
        image.height = 1;
        image.pixels = {200, 100, 50, 255, 0, 0, 0, 0};
        const Pixel mean = meanColour(image, Pixel{1, 2, 3});
        assert(mean.r == 200 && mean.g == 100 && mean.b == 50);

        image.pixels = {200, 100, 50, 255, 100, 50, 0, 255};
        assert(meanColour(image, Pixel{}).r == 150 && meanColour(image, Pixel{}).b == 25);

        image.pixels = {200, 100, 50, 0, 0, 0, 0, 0};
        assert(meanColour(image, Pixel{1, 2, 3}).g == 2);
    }

    // Its settings: every field named once, all read at start-up; understorey palms' sizes; shadows in the nearest
    // cascades only; its card from a few tens of metres.
    void testThePalmSettings() {
        ForestPalmSettings settings;
        FieldCounter counter;
        visit(settings, counter);
        assert(counter.fields == 24 && counter.live == 0);
        assert(settings.shortest >= 1.5f && settings.tallest <= 6.0f && settings.shortest < settings.tallest);
        assert(settings.leastFronds <= settings.mostFronds && settings.variants >= 4);
        // Shadows in every cascade (0), as the understorey's, so they do not pop in as one walks up.
        assert(settings.shadowCascades == 0);
        assert(settings.impostorErrorMetres > 0.0f && settings.impostorErrorMetres < ForestTreeSettings{}.impostorErrorMetres);
    }

    /*
    * The game plants them: each palm its seen mesh (casting no shadow) and its shadow's stand-in (shadows only, in the
    * cascades its settings name), with levels ending where its card begins; an opaque, double-sided leaf letting light through,
    * placed, turned and scaled as its placement says.
    */
    void testTheGamePlantsThePalms() {
        TestForest forest;
        World& world = forest.world;
        ForestGame& game = forest.game;
        const std::vector<PalmPlacement>& placed = game.getPalmPlacements();
        const std::vector<Entity>& palms = game.getPalms();
        assert(!placed.empty() && palms.size() == 2 * placed.size() && game.getPalmImpostors().size() == placed.size());

        for (std::size_t palm = 0; palm < placed.size(); palm += 23) {
            const ModelRenderer& seen = world.get<ModelRenderer>(palms[2 * palm]);
            const ModelRenderer& shadow = world.get<ModelRenderer>(palms[2 * palm + 1]);
            assert(!seen.castsShadows && !seen.shadowOnly && shadow.castsShadows && shadow.shadowOnly);
            assert(shadow.shadowCascades == ForestPalmSettings{}.shadowCascades);

            const Material& leaf = seen.parts.front().material;
            assert(!isCutOut(leaf) && leaf.doubleSided && leaf.texture.isValid() && leaf.normalTexture.isValid());
            assert(leaf.diffuseTransmission == ForestLeafLightSettings{}.share);

            const std::vector<MeshLevel>& levels = forest.assets.getMeshLevels().at(seen.parts.front().mesh.index);
            // Thinned leaflet by leaflet into its kept leaflets (its own first vertices), then its card; the shadow's stand-in
            // is never seen.
            assert(levels.size() == 2 && levels[0].sharesVertices && levels[0].mesh != shadow.parts.front().mesh);
            assert(!levels[1].mesh.isValid() && levels[1].errorMetres >= ForestPalmSettings{}.impostorErrorMetres && levels[1].errorMetres >= levels[0].errorMetres);

            const ModelRenderer& card = world.get<ModelRenderer>(game.getPalmImpostors()[palm]);
            const MeshLevelOptions& cardOptions = forest.assets.getMeshLevelOptions().at(card.parts.front().mesh.index);
            assert(cardOptions.hiddenBelowError == levels[1].errorMetres && !card.castsShadows && isCutOut(card.parts.front().material));
            assert(card.parts.front().material.diffuseTransmission == leaf.diffuseTransmission);

            const Transform& pose = world.get<Transform>(palms[2 * palm]);
            assert(pose.position.x == placed[palm].foot.x && pose.position.z == placed[palm].foot.z && pose.scale.y == placed[palm].scale);
            assert(world.get<Transform>(game.getPalmImpostors()[palm]).position.x == placed[palm].foot.x);
        }
    }
}

void testPalms() {
    testAFrondBendsLikeATaperingCantilever();
    testTheLeafletsOutlines();
    testAFrondsSpine();
    testThePalmVariants();
    testAPalmIsGrown();
    testAPalmThinsLeafletByLeaflet();
    testTheUnderstoreyStandsOnItsCells();
    testThePalmsArePlaced();
    testTheLeafletsPictures();
    testAMeanColourCountsOnlyTheLeaves();
    testThePalmSettings();
    testTheGamePlantsThePalms();
}
