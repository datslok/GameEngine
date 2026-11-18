#include "assets/asset_manager.h"
#include "core/settings_visitor.h"
#include "engine/debug_views.h"
#include "engine/settings_file.h"
#include "render/gpu/render_settings.h"
#include "render/render_stats.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/level_fade.h"
#include "scene/frame_description.h"
#include "scene/level_of_detail.h"
#include "scene/mesh.h"
#include "scene/simplify_mesh.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    // A view 1000 pixels high with a 90 degree field of view: 500 pixels a metre at a metre, so the numbers are easy.
    LevelView squareView(const Vec3& eye, float pixelError) {
        return levelView(eye, 1000.0f, 1.5707963f, LevelOfDetailSettings{.enabled = true, .pixelError = pixelError});
    }

    // Two coarser levels: one 1 cm out, one 10 cm out.
    const std::vector<MeshLevel> twoLevels{MeshLevel{MeshHandle{7}, 0.01f}, MeshLevel{MeshHandle{8}, 0.1f}};

    // A draw of base mesh 3 with the two levels, its sphere 1 m round centre.
    DrawItem drawAt(const Vec3& centre) {
        DrawItem draw;
        draw.mesh = MeshHandle{3};
        draw.bounds = BoundingSphere{centre, 1.0f};
        draw.levels = twoLevels;
        return draw;
    }

    /*
    * A view's pixels a metre at a metre are its height over the height a metre away covers, 2 tan(fov / 2): a metre's
    * error d metres away covers that over d pixels. A narrower view (a zoom) sees more pixels in the same error.
    */
    void testAnErrorCoversPixelsByDistance() {
        const LevelView view = squareView(Vec3{0.0f, 0.0f, 0.0f}, 2.0f);
        assert(near(view.pixelsPerMetre, 500.0f, 0.01f));
        assert(near(errorInPixels(0.01f, 10.0f, view), 0.5f, 1e-4f));
        assert(near(errorInPixels(0.1f, 10.0f, view), 5.0f, 1e-3f));

        const LevelView zoomed = levelView(Vec3{}, 1000.0f, 0.5f, LevelOfDetailSettings{});
        assert(zoomed.pixelsPerMetre > 3.0f * view.pixelsPerMetre);

        // Standing on it, any error covers everything.
        assert(errorInPixels(0.01f, 0.0f, view) == std::numeric_limits<float>::infinity());

        // An orthographic view (the sun's shadow tiles) has the same pixels a metre everywhere: distance does not count.
        const LevelView flat{.eye = Vec3{}, .pixelsPerMetre = 8.0f, .pixelError = 2.0f, .enabled = true, .orthographic = true};
        assert(errorInPixels(0.5f, 1.0f, flat) == 4.0f && errorInPixels(0.5f, 1000.0f, flat) == 4.0f && errorInPixels(0.5f, 0.0f, flat) == 4.0f);
        assert(chooseLevel(twoLevels, 0.0f, flat) == 2);
    }

    /*
    * The coarsest level whose error covers no more than the budget's pixels: the mesh itself (0) close up, the 1 cm
    * level from 2.5 m (0.01 x 500 / 2), the 10 cm level from 25 m. No levels, or levels off, is always the mesh itself.
    */
    void testTheCoarsestLevelWithinTheBudgetIsChosen() {
        const LevelView view = squareView(Vec3{}, 2.0f);
        assert(chooseLevel(twoLevels, 1.0f, view) == 0);
        assert(chooseLevel(twoLevels, 2.4f, view) == 0);
        assert(chooseLevel(twoLevels, 2.6f, view) == 1);
        assert(chooseLevel(twoLevels, 24.0f, view) == 1);
        assert(chooseLevel(twoLevels, 26.0f, view) == 2);
        assert(chooseLevel(twoLevels, 1e6f, view) == 2);
        assert(chooseLevel({}, 1e6f, view) == 0);

        // A bigger budget picks coarser levels nearer.
        assert(chooseLevel(twoLevels, 10.0f, squareView(Vec3{}, 4.0f)) == 1);
        assert(chooseLevel(twoLevels, 10.0f, squareView(Vec3{}, 8.0f)) == 2);

        LevelOfDetailSettings off;
        off.enabled = false;
        assert(chooseLevel(twoLevels, 1e6f, levelView(Vec3{}, 1000.0f, 1.5707963f, off)) == 0);
    }

    /*
    * A draw is measured from its nearest point, the surface of its sphere (inside it, 0: the mesh itself), so a big
    * tree's near side never shows a level picked for its middle. Unknown bounds (a negative radius) keep the mesh.
    */
    void testADrawPicksByItsNearestPoint() {
        const LevelView view = squareView(Vec3{}, 2.0f);
        assert(near(levelDistance(BoundingSphere{Vec3{0.0f, 0.0f, -30.0f}, 1.0f}, Vec3{}), 29.0f, 1e-4f));
        assert(levelDistance(BoundingSphere{Vec3{0.5f, 0.0f, 0.0f}, 1.0f}, Vec3{}) == 0.0f);

        assert(chosenMesh(drawAt(Vec3{0.0f, 0.0f, -30.0f}), view) == MeshHandle{8});
        assert(chosenMesh(drawAt(Vec3{0.0f, 0.0f, -10.0f}), view) == MeshHandle{7});
        assert(chosenMesh(drawAt(Vec3{0.0f, 0.0f, -2.0f}), view) == MeshHandle{3});
        assert(chosenMesh(drawAt(Vec3{0.0f, 0.0f, 0.0f}), view) == MeshHandle{3});

        DrawItem unknown = drawAt(Vec3{0.0f, 0.0f, -100.0f});
        unknown.bounds.radius = -1.0f;
        assert(chosenMesh(unknown, view) == MeshHandle{3});

        DrawItem plain = drawAt(Vec3{0.0f, 0.0f, -100.0f});
        plain.levels = {};
        assert(chosenMesh(plain, view) == MeshHandle{3});
    }

    /*
    * Batching for a view groups the draws by the mesh each picks there, so one shape near and far is two instanced draws:
    * the instances stay the draws' own (their model matrices), only the mesh bound for them changes. Without a view,
    * every draw is its own mesh, as before.
    */
    void testBatchingDrawsEachDrawsChosenMesh() {
        const std::vector<DrawItem> draws{drawAt(Vec3{0.0f, 0.0f, -100.0f}), drawAt(Vec3{0.0f, 0.0f, -2.0f}), drawAt(Vec3{0.0f, 0.0f, -10.0f}),
                                          drawAt(Vec3{0.0f, 0.0f, -200.0f})};
        const LevelView view = squareView(Vec3{}, 2.0f);
        const BatchedDraws batched = batchByMesh(draws, {0, 1, 2, 3}, 0, &view);
        assert(batched.batches.size() == 3);
        assert(batched.batches[0].mesh == MeshHandle{3} && batched.batches[0].instanceCount == 1);
        assert(batched.batches[1].mesh == MeshHandle{7} && batched.batches[1].instanceCount == 1);
        assert(batched.batches[2].mesh == MeshHandle{8} && batched.batches[2].instanceCount == 2);
        assert((batched.order == std::vector<std::size_t>{1, 2, 0, 3}));

        const BatchedDraws plain = batchByMesh(draws, {0, 1, 2, 3});
        assert(plain.batches.size() == 1 && plain.batches[0].mesh == MeshHandle{3} && plain.batches[0].instanceCount == 4);

        // The readout's count of each level a view drew.
        const std::array<std::size_t, meshLevelSlots> counts = levelsChosen(draws, {0, 1, 2, 3}, view);
        assert(counts[0] == 1 && counts[1] == 1 && counts[2] == 2 && counts[3] == 0);
    }

    // Each draw gets its mesh's levels from a table indexed like MeshHandle; meshes without any get none.
    void testDrawsGetTheirMeshesLevels() {
        std::vector<DrawItem> draws(3);
        draws[0].mesh = MeshHandle{1};
        draws[1].mesh = MeshHandle{0};
        draws[2].mesh = MeshHandle{5};
        const std::vector<std::vector<MeshLevel>> table{{}, twoLevels};
        attachMeshLevels(draws, table);
        assert(draws[0].levels.size() == 2 && draws[0].levels[1].mesh == MeshHandle{8});
        assert(draws[1].levels.empty() && draws[2].levels.empty());
    }

    /*
    * A game names a mesh's coarser levels on the asset manager: at most meshLevelSlots - 1, each coarser than the last
    * (a larger error), each another mesh than the base. Anything else is a mistake and throws.
    */
    void testTheAssetManagerHoldsEachMeshsLevels() {
        AssetManager assets;
        const MeshHandle base = assets.addMesh(Mesh::cube());
        const MeshHandle coarse = assets.addMesh(Mesh::cube());
        const MeshHandle coarser = assets.addMesh(Mesh::cube());
        assets.setMeshLevels(base, {MeshLevel{coarse, 0.01f}, MeshLevel{coarser, 0.05f}});
        assert(assets.getMeshLevels().size() > base.index && assets.getMeshLevels()[base.index].size() == 2);
        assert(assets.getMeshLevels()[base.index][1].mesh == coarser);

        bool threw = false;
        try {
            assets.setMeshLevels(coarse, {MeshLevel{coarser, 0.05f}, MeshLevel{base, 0.01f}});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);

        threw = false;
        try {
            assets.setMeshLevels(base, {MeshLevel{coarse, 0.01f}, MeshLevel{coarse, 0.02f}, MeshLevel{coarse, 0.03f}, MeshLevel{coarse, 0.04f}});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);

        threw = false;
        try {
            assets.setMeshLevels(base, {MeshLevel{MeshHandle{}, 0.01f}, MeshLevel{coarse, 0.02f}});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);

        // A last level without a mesh draws nothing there (a tree past where its impostor takes over).
        assets.setMeshLevels(base, {MeshLevel{coarse, 0.01f}, MeshLevel{MeshHandle{}, 0.5f}});
        assert(!assets.getMeshLevels()[base.index][1].mesh.isValid());

        // Options: an impostor hidden near (its error not below 0), the box a tree's parts are chosen by.
        assets.setMeshLevelOptions(coarser, MeshLevelOptions{.hiddenBelowError = 0.5f, .chooseBounds = Aabb{Vec3{-1.0f, 0.0f, -1.0f}, Vec3{1.0f, 4.0f, 1.0f}}});
        assert(assets.getMeshLevelOptions().at(coarser.index).hiddenBelowError == 0.5f);
        threw = false;
        try {
            assets.setMeshLevelOptions(coarser, MeshLevelOptions{.hiddenBelowError = -1.0f, .chooseBounds = std::nullopt});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    /*
    * The simplifier says how far it moved the surface, in metres (meshoptimizer's error is a share of the mesh's size):
    * the level's error to choose it by. A 1 m rippled sheet simplified within 2% moved it no more than 2 cm.
    */
    void testSimplifyingReportsItsErrorInMetres() {
        IndexedMesh sheet = buildIndexedMesh(Mesh::grid(40, 40, 1.0f, 1.0f));

        for (MeshVertex& vertex : sheet.vertices) {
            vertex.position.z = 0.05f * std::sin(6.0f * vertex.position.x) * std::cos(4.0f * vertex.position.y);
        }

        float errorMetres = -1.0f;
        const IndexedMesh simpler = simplifyMesh(sheet, 0.1f, 0.02f, false, &errorMetres);
        assert(simpler.indices.size() < sheet.indices.size());
        assert(errorMetres > 0.0f && errorMetres <= 0.02f * 1.0f * 1.01f);

        float none = -1.0f;
        simplifyMesh(sheet, 1.0f, 0.0f, false, &none);
        assert(none == 0.0f);
    }

    /*
    * A generated shape's levels in one call: each a share of its triangles, borders free (a closed rock has none),
    * within the error allowed, coarser each time, their errors rising; the asset manager adds the mesh and its levels
    * together.
    */
    void testAShapesLevelsAreSimplifiedFromIt() {
        IndexedMesh sheet = buildIndexedMesh(Mesh::grid(40, 40, 1.0f, 1.0f));

        for (MeshVertex& vertex : sheet.vertices) {
            vertex.position.z = 0.05f * std::sin(6.0f * vertex.position.x) * std::cos(4.0f * vertex.position.y);
        }

        const std::vector<SimplifiedLevel> levels = simplifiedLevels(sheet, {0.25f, 0.05f}, 0.05f);
        assert(levels.size() == 2);
        assert(levels[0].mesh.indices.size() < sheet.indices.size() && levels[1].mesh.indices.size() < levels[0].mesh.indices.size());
        assert(levels[0].errorMetres > 0.0f && levels[1].errorMetres >= levels[0].errorMetres);

        AssetManager assets;
        const std::size_t before = assets.getMeshCount();
        const MeshHandle base = assets.addDrawnMeshWithLevels(sheet, levels);
        assert(assets.getMeshCount() == before + 3 && assets.isDrawnOnly(base));
        const std::vector<MeshLevel>& named = assets.getMeshLevels().at(base.index);
        assert(named.size() == 2 && named[1].errorMetres == levels[1].errorMetres && assets.isDrawnOnly(named[0].mesh));
    }

    /*
    * Impostors (docs/research/LOD.md): a tree's parts end on a level without a mesh, drawing nothing past it; its impostor is
    * hidden until the same error fits, so at every distance exactly one of them is drawn. Both choose by one shared box
    * (MeshLevelOptions::chooseBounds), placed by each draw's model matrix, so they switch at the same place; batching
    * leaves out a draw drawing nothing.
    */
    void testAnImpostorTakesOverWhereATreeEnds() {
        const std::vector<MeshLevel> treeLevels{MeshLevel{MeshHandle{7}, 0.01f}, MeshLevel{MeshHandle{}, 0.1f}};
        const std::vector<std::vector<MeshLevel>> table{{}, {}, {}, treeLevels};
        std::vector<MeshLevelOptions> options(10);
        const Aabb treeBox{Vec3{-2.0f, 0.0f, -2.0f}, Vec3{2.0f, 10.0f, 2.0f}};
        options[3] = MeshLevelOptions{.chooseBounds = treeBox};
        options[9] = MeshLevelOptions{.hiddenBelowError = 0.1f, .chooseBounds = treeBox};

        const LevelView view = squareView(Vec3{}, 2.0f);
        int drawnSomewhere = 0;

        for (float distance = 5.0f; distance < 60.0f; distance += 0.37f) {
            std::vector<DrawItem> draws(2);
            draws[0].mesh = MeshHandle{3};
            draws[1].mesh = MeshHandle{9};

            for (DrawItem& draw : draws) {
                draw.model = Mat4::translation(0.0f, 0.0f, -distance);
                draw.bounds = BoundingSphere{Vec3{0.0f, 1.0f, -distance}, 1.0f}; // each mesh's own, unlike the shared box
            }

            attachMeshLevels(draws, table, options);
            assert(draws[0].levelBounds.radius > 5.0f && draws[1].levelBounds.centre.y == 5.0f);
            const bool tree = chosenMesh(draws[0], view).isValid();
            const bool impostor = chosenMesh(draws[1], view).isValid();
            assert(tree != impostor);
            drawnSomewhere += impostor ? 1 : 0;

            const BatchedDraws batched = batchByMesh(draws, {0, 1}, 0, &view);
            assert(batched.order.size() == 1 && batched.batches.size() == 1 && batched.batches[0].mesh.isValid());
        }

        assert(drawnSomewhere > 0 && drawnSomewhere < 140);

        // With levels off, the tree is drawn whole and its impostor never.
        LevelOfDetailSettings off;
        off.enabled = false;
        std::vector<DrawItem> far(2);
        far[0].mesh = MeshHandle{3};
        far[1].mesh = MeshHandle{9};
        far[0].bounds = far[1].bounds = BoundingSphere{Vec3{0.0f, 1.0f, -500.0f}, 1.0f};
        far[0].model = far[1].model = Mat4::translation(0.0f, 0.0f, -500.0f);
        attachMeshLevels(far, table, options);
        const LevelView offView = levelView(Vec3{}, 1000.0f, 1.5707963f, off);
        assert(chosenMesh(far[0], offView) == MeshHandle{3} && !chosenMesh(far[1], offView).isValid());

        // With the cards alone off, the tree keeps its last level with a mesh however far, and its impostor is never
        // drawn; the other levels still count.
        LevelOfDetailSettings noCards;
        noCards.impostors = false;
        const LevelView noCardsView = levelView(Vec3{}, 1000.0f, 1.5707963f, noCards);
        assert(!noCardsView.impostors);
        assert(chosenMesh(far[0], noCardsView) == MeshHandle{7} && !chosenMesh(far[1], noCardsView).isValid());
        assert(drawLevel(far[0], noCardsView) == 1);
        assert(chooseLevel(treeLevels, 1e6f, noCardsView) == 1);
    }

    /*
    * The square view switches the 1 cm level in at 2.5 m and the 10 cm one at 25 m (from the sphere's surface). With a
    * fade band of 0.2, a draw that fades draws both levels over the fifth of each distance before its switch, the share
    * running 0 to 1 across it by the coarser level's error, and one level everywhere else. Draws that do not fade, views
    * without a band (captures) and orthographic views (shadow tiles) never fade.
    */
    void testAFadingDrawCrossesTheBand() {
        LevelView view = cameraLevelView(Vec3{}, 1000.0f, 1.5707963f, LevelOfDetailSettings{.enabled = true, .pixelError = 2.0f, .fadeBand = 0.2f});
        assert(view.fadeBand == 0.2f);
        assert(levelView(Vec3{}, 1000.0f, 1.5707963f, LevelOfDetailSettings{.fadeBand = 0.2f}).fadeBand == 0.0f);

        // 22.5 m out: halfway through the band before 25 m, where 10 cm covers 2.22 pixels and 2 / 2.22 = 0.9.
        DrawItem draw = drawAt(Vec3{0.0f, 0.0f, -23.5f});
        assert(!levelFade(draw, view).fading);
        draw.fadesLevels = true;
        const LevelFade halfway = levelFade(draw, view);
        assert(halfway.fading && halfway.finer == MeshHandle{7} && halfway.coarser == MeshHandle{8});
        assert(near(halfway.share, 0.5f, 1e-3f));
        assert(halfway.finer == chosenMesh(draw, view));

        // Outside the bands, one level; through a band the share only grows.
        for (const float distance : {1.5f, 2.6f, 19.0f, 26.0f, 60.0f}) {
            assert(!levelFade(drawAt(Vec3{0.0f, 0.0f, -1.0f - distance}), view).fading);
            DrawItem outside = drawAt(Vec3{0.0f, 0.0f, -1.0f - distance});
            outside.fadesLevels = true;
            assert(!levelFade(outside, view).fading);
        }

        float last = -1.0f;

        for (float distance = 20.05f; distance < 25.0f; distance += 0.1f) {
            DrawItem inside = drawAt(Vec3{0.0f, 0.0f, -1.0f - distance});
            inside.fadesLevels = true;
            const LevelFade fade = levelFade(inside, view);
            assert(fade.fading && fade.share > last && fade.share >= 0.0f && fade.share <= 1.0f);
            last = fade.share;
        }

        assert(last > 0.95f);

        // The first band, before 2.5 m: the mesh itself into the 1 cm level.
        DrawItem nearer = drawAt(Vec3{0.0f, 0.0f, -3.25f});
        nearer.fadesLevels = true;
        const LevelFade first = levelFade(nearer, view);
        assert(first.fading && first.finer == MeshHandle{3} && first.coarser == MeshHandle{7} && near(first.share, 0.5f, 1e-3f));

        // A capture's view and an orthographic one fade nothing; nor does a band of 0.
        assert(!levelFade(draw, levelView(Vec3{}, 1000.0f, 1.5707963f, LevelOfDetailSettings{})).fading);
        LevelView flat = view;
        flat.orthographic = true;
        assert(!levelFade(draw, flat).fading);
        LevelView noBand = view;
        noBand.fadeBand = 0.0f;
        assert(!levelFade(draw, noBand).fading);

        // Batched, the fading draw is drawn twice, once with each level, the two entries marked as the finer and coarser.
        const std::vector<DrawItem> draws{draw};
        const BatchedDraws batched = batchByMesh(draws, {0}, 0, &view);
        assert(batched.batches.size() == 2 && batched.order.size() == 2 && batched.marks.size() == 2);
        assert(batched.batches[0].mesh == MeshHandle{7} && batched.batches[1].mesh == MeshHandle{8});
        assert(batched.marks[0] == levelFadeField(halfway.share, false) << drawListFadeShift);
        assert(batched.marks[1] == levelFadeField(halfway.share, true) << drawListFadeShift);

        // The readout counts both levels drawn.
        const std::array<std::size_t, meshLevelSlots> counts = levelsChosen(draws, {0}, view);
        assert(counts[0] == 0 && counts[1] == 1 && counts[2] == 1);
    }

    /*
    * A plant fading into its card: the plant's last level draws nothing from the card's error on, so in the band before
    * it the plant keeps its level (fading out, no coarser mesh) while its card fades in (no finer one), by the same share.
    */
    void testAPlantFadesIntoItsCard() {
        const std::vector<MeshLevel> plantLevels{MeshLevel{MeshHandle{7}, 0.01f}, MeshLevel{MeshHandle{}, 0.1f}};
        const std::vector<std::vector<MeshLevel>> table{{}, {}, {}, plantLevels};
        std::vector<MeshLevelOptions> options(10);
        const Aabb box{Vec3{-0.5f, 0.0f, -0.5f}, Vec3{0.5f, 1.0f, 0.5f}};
        options[3] = MeshLevelOptions{.chooseBounds = box, .fadesLevels = true};
        options[9] = MeshLevelOptions{.hiddenBelowError = 0.1f, .chooseBounds = box, .fadesLevels = true};
        const LevelView view = cameraLevelView(Vec3{}, 1000.0f, 1.5707963f, LevelOfDetailSettings{.enabled = true, .pixelError = 2.0f, .fadeBand = 0.2f});

        std::vector<DrawItem> draws(2);
        draws[0].mesh = MeshHandle{3};
        draws[1].mesh = MeshHandle{9};

        for (DrawItem& draw : draws) {
            draw.model = Mat4::translation(0.0f, 0.0f, -23.0f);
            draw.bounds = BoundingSphere{Vec3{0.0f, 0.5f, -23.0f}, 1.0f};
        }

        attachMeshLevels(draws, table, options);
        assert(draws[0].fadesLevels && draws[1].fadesLevels);
        const LevelFade plant = levelFade(draws[0], view);
        const LevelFade card = levelFade(draws[1], view);
        assert(plant.fading && plant.finer == MeshHandle{7} && !plant.coarser.isValid());
        assert(card.fading && !card.finer.isValid() && card.coarser == MeshHandle{9});
        assert(plant.share == card.share && plant.share > 0.0f && plant.share < 1.0f);

        // Batched: the plant's level marked finer, its card coarser, with one field step between them only by the bit.
        const BatchedDraws batched = batchByMesh(draws, {0, 1}, 0, &view);
        assert(batched.order.size() == 2);
        assert((batched.marks[0] ^ batched.marks[1]) == drawListFadeCoarse << drawListFadeShift);
    }

    // The settings: on, a budget of pixels, a fade band, all live; a budget not above 0, or a band outside 0 to 0.9, is refused.
    void testTheSettingsAreChecked() {
        validateLevelOfDetail(LevelOfDetailSettings{});
        bool threw = false;
        try {
            validateLevelOfDetail(LevelOfDetailSettings{.enabled = true, .pixelError = 0.0f});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        // A fade band from 0 to 0.9 only.
        for (const float band : {-0.1f, 0.95f}) {
            bool refused = false;

            try {
                validateLevelOfDetail(LevelOfDetailSettings{.fadeBand = band});
            } catch (const std::invalid_argument&) {
                refused = true;
            }

            assert(refused);
        }

        assert(threw);

        // Settable by its settings.cfg key, as every setting is (the command line's set and get).
        RenderSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };
        assert(setSetting(walk, "levelOfDetail.pixelError", "6").outcome == SettingOutcome::Set);
        assert(setSetting(walk, "levelOfDetail.enabled", "false").outcome == SettingOutcome::Set);
        assert(setSetting(walk, "levelOfDetail.impostors", "false").outcome == SettingOutcome::Set);
        assert(setSetting(walk, "levelOfDetail.fadeBand", "0.3").outcome == SettingOutcome::Set && settings.levelOfDetail.fadeBand == 0.3f);
        assert(!settings.levelOfDetail.impostors && LevelOfDetailSettings{}.impostors);
        assert(!settings.levelOfDetail.enabled);
        assert(settings.levelOfDetail.pixelError == 6.0f);
    }

    // The F4 readout (and the stats command) shows how many of the camera's draws drew each level, and the
    // thousands of triangles drawn.
    void testTheReadoutCountsEachLevel() {
        RenderStats stats;
        stats.drawnByLevel = {5, 3, 2, 0};
        stats.drawnTriangles = 1234567;
        stats.drawnCutOutTriangles = 800000;
        stats.probeTriangles = 4500;
        assert(renderStatsLines(stats)[1].find("LEVELS 5 3 2 0") != std::string::npos);
        assert(renderStatsLines(stats)[1].find("TRIS 1234K  CUT 800K  PROBE 4K") != std::string::npos);
    }
}

void testLevelOfDetail() {
    testAnImpostorTakesOverWhereATreeEnds();
    testAFadingDrawCrossesTheBand();
    testAPlantFadesIntoItsCard();
    testTheReadoutCountsEachLevel();
    testAnErrorCoversPixelsByDistance();
    testTheCoarsestLevelWithinTheBudgetIsChosen();
    testADrawPicksByItsNearestPoint();
    testBatchingDrawsEachDrawsChosenMesh();
    testDrawsGetTheirMeshesLevels();
    testTheAssetManagerHoldsEachMeshsLevels();
    testSimplifyingReportsItsErrorInMetres();
    testAShapesLevelsAreSimplifiedFromIt();
    testTheSettingsAreChecked();
}
