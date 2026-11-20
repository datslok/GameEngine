#include "games/forest/understorey.h"
#include "games/forest/forest_game.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/forest_trees.h"
#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "scene/model_renderer.h"

#include "forest_helpers.h"
#include "test_levels.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <vector>

namespace {
    /*
    * The understorey's places (docs/research/JUNGLE.md, The understorey): on the trees' stretch of the floor, clear of the path,
    * on the ground, no plant blocking a sight line; every kind, each scaled within its range; thick near the path and
    * thinning away from it; the same every run.
    */
    void testTheUnderstoreyIsPlaced() {
        const ForestUnderstoreySettings settings;
        const std::vector<SightLine> lines = forestSightLines();
        const std::vector<UnderstoreyPlacement>& plants = defaultPatchPlanting().understorey;
        assert(plants.size() > 1000);
        std::array<int, understoreyKindCount> kinds{};
        float nearCount = 0.0f;
        float farCount = 0.0f;

        for (const UnderstoreyPlacement& plant : plants) {
            assert(plant.foot.z >= treeNorthmost && plant.foot.z <= treeSouthmost);
            assert(std::fabs(plant.foot.x - valleyAxis(plant.foot.z)) <= treeSpread);
            const float fromPath = forestPathDistance(plant.foot.x, plant.foot.z);
            assert(fromPath >= settings.planting.pathClearance - 1e-3f);
            assert(plant.foot.y == terrainSurfaceHeight(patchTerrainHeights(), plant.foot.x, plant.foot.z) - understoreySinkMetres);
            assert(plant.variantPick >= 0.0f && plant.variantPick < 1.0f);
            assert(plant.scale >= 1.0f - understoreyScaleSpread - 1e-5f && plant.scale <= 1.0f + understoreyScaleSpread + 1e-5f);
            assert(!blocksSight(understoreyCentre(settings, plant), understoreyRadius(settings, plant), lines));
            ++kinds[static_cast<std::size_t>(plant.kind)];
            nearCount += fromPath < settings.planting.nearPathMetres ? 1.0f : 0.0f;
            farCount += fromPath > settings.planting.farPathMetres ? 1.0f : 0.0f;
        }

        for (int count : kinds) {
            assert(count > 100);
        }

        // Thicker near the path: there the plants per grid cell are near one; far out, about farShare of that.
        assert(nearCount > 0.0f && farCount > 0.0f);

        const ForestPlanting again = plantThePatch(defaultSpecies(), patchValleyMaps(), patchTerrainHeights(), testJobs());
        assert(again.understorey.size() == plants.size() && again.understorey.back().foot.x == plants.back().foot.x &&
               again.understorey.back().kind == plants.back().kind);
    }

    // The near share is all of the lattice's cells there, the far one farShare of them: compared per square metre on a
    // wide stretch with no trunks, obstacles, sight lines or gaps.
    void testTheUnderstoreyThinsAwayFromThePath() {
        ForestUnderstoreySettings settings;
        settings.planting.gapShare = 1.0f;
        const ValleyMaps open = buildValleyMaps(testPlantingSettings(), {}, {}, {understoreyReach(settings)}, testJobs());
        const auto alone = [&](const ForestUnderstoreySettings& asked) {
            std::vector<Vec3> feet;

            for (const PlantedPlant& plant : plantAlone(understoreyPlantingLayer(asked), open)) {
                feet.push_back(plant.foot);
            }

            return feet;
        };
        const std::vector<Vec3> plants = alone(settings);
        int near = 0;
        int far = 0;

        for (const Vec3& foot : plants) {
            const float fromPath = forestPathDistance(foot.x, foot.z);
            near += fromPath >= settings.planting.pathClearance + settings.planting.footprintMetres && fromPath < settings.planting.nearPathMetres ? 1 : 0;
            far += fromPath > settings.planting.farPathMetres ? 1 : 0;
        }

        // Cells near the path are all kept, far ones about farShare of them; both
        // bands' areas are measured by the grid's own cells, counted with nothing kept out.
        ForestUnderstoreySettings everything = settings;
        everything.planting.farShare = 1.0f;
        int nearCells = 0;
        int farCells = 0;

        for (const Vec3& foot : alone(everything)) {
            const float fromPath = forestPathDistance(foot.x, foot.z);
            nearCells += fromPath >= settings.planting.pathClearance + settings.planting.footprintMetres && fromPath < settings.planting.nearPathMetres ? 1 : 0;
            farCells += fromPath > settings.planting.farPathMetres ? 1 : 0;
        }

        const float nearShare = static_cast<float>(near) / static_cast<float>(nearCells);
        const float farShare = static_cast<float>(far) / static_cast<float>(farCells);
        assert(nearShare > 0.95f && farShare > settings.planting.farShare - 0.1f && farShare < settings.planting.farShare + 0.1f);
    }

    /*
    * A kind's scale against its scan: its tallest plant brought to the kind's height, the others scaled alike, so they
    * stay smaller.
    */
    void testAKindsTallestPlantSetsItsScale() {
        IndexedMesh tall = buildIndexedMesh(Mesh::grid(2, 2, 1.0f, 0.4f));
        IndexedMesh small = buildIndexedMesh(Mesh::grid(2, 2, 1.0f, 0.1f));
        const std::vector<IndexedMesh> plants{footedPlant(small), footedPlant(tall)};
        assert(near(understoreyKindScale(plants, 1.6f), 4.0f, 1e-5f));
        assert(understoreyKindScale({}, 1.6f) == 1.0f);
    }

    // A scanned plant stood on its foot: its lowest point at the ground, centred over the origin.
    void testAPlantStandsOnItsFoot() {
        IndexedMesh plant = buildIndexedMesh(Mesh::grid(4, 4, 1.0f, 2.0f));

        for (MeshVertex& vertex : plant.vertices) {
            vertex.position = vertex.position + Vec3{3.0f, 5.0f, -2.0f};
        }

        const IndexedMesh stood = footedPlant(plant);
        const Aabb bounds = indexedMeshBounds(stood);
        assert(near(bounds.minimum.y, 0.0f, 1e-5f) && near(bounds.maximum.y, 2.0f, 1e-5f));
        assert(near(bounds.minimum.x + bounds.maximum.x, 0.0f, 1e-5f) && near(bounds.minimum.z + bounds.maximum.z, 0.0f, 1e-5f));
    }

    /*
    * Its settings: every field named once, all read at start-up. Each kind's tallest plant is a jungle plant's height
    * (1-2 m: the scans are house plants 0.2-0.5 m tall).
    */
    void testTheUnderstoreySettings() {
        ForestUnderstoreySettings settings;
        assert(settings.keepShare > settings.shadowKeepShare && settings.planting.farShare < 1.0f);

        for (int kind = 0; kind < understoreyKindCount; ++kind) {
            const float height = understoreyHeight(settings, static_cast<UnderstoreyKind>(kind));
            assert(height >= 1.0f && height <= 2.0f);
        }

        FieldCounter counter;
        visit(settings, counter);
        assert(counter.fields == 23 && counter.live == 0);

        // Its cards (scene/impostor.h) take over a few tens of metres away (under 15 cm's error: about 60 m at 2 pixels),
        // and its frames have fewer texels than a tree's, never big on screen.
        assert(settings.impostorErrorMetres > 0.0f && settings.impostorErrorMetres < 0.15f);
        assert(settings.impostorFrames >= 4 && settings.impostorTexels > 0 && settings.impostorTexels < ForestTreeSettings{}.impostorTexels);

        // Its shadows in every cascade (0): stopped at the nearest two, they popped in plant by plant about 20 m away.
        assert(settings.shadowCascades == 0);
    }

    /*
    * The game draws them on the GPU (stand-ins without the scans): a look for each kind's variant, in the variants'
    * order, the layer's pick choosing among them as each plant's did; each its leaves, seen and casting no shadow,
    * swaying, its shadow's stand-in, drawn into shadows only, and its card, still, taking over far away; no entity a
    * plant. The plants themselves clear of every tree's trunk.
    */
    void testTheGamePlantsTheUnderstorey() {
        TestForest forest;
        ForestGame& game = forest.game;
        const std::vector<UnderstoreyPlacement>& placed = game.getUnderstoreyPlacements();
        assert(!placed.empty());
        std::array<int, understoreyKindCount> variants{};

        for (const PlantedKindLook& look : game.getPlantLooks()) {
            if (look.layer != static_cast<int>(ForestLayer::Understorey)) {
                continue;
            }

            assert(look.kind >= 0 && look.kind < understoreyKindCount && look.variant == variants[static_cast<std::size_t>(look.kind)]);
            ++variants[static_cast<std::size_t>(look.kind)];
            assert(look.models.size() == 3);
            const ModelRenderer& leaves = look.models[0];
            const ModelRenderer& shadow = look.models[1];
            const ModelRenderer& card = look.models[2];
            assert(!leaves.castsShadows && !leaves.shadowOnly && shadow.castsShadows && shadow.shadowOnly);
            assert(shadow.shadowCascades == ForestUnderstoreySettings{}.shadowCascades);
            assert(leaves.sway.kind != SwayKind::Still && shadow.sway.kind == leaves.sway.kind && card.sway.kind == SwayKind::Still);

            // The leaves seen have two coarser levels (docs/research/LOD.md): simplified further, then the shadow's own stand-in,
            // then nothing, where the card begins.
            const std::vector<MeshLevel>& levels = forest.assets.getMeshLevels().at(leaves.parts.front().mesh.index);
            assert(levels.size() == 3 && levels[1].mesh == shadow.parts.front().mesh && levels[1].errorMetres >= levels[0].errorMetres);
            assert(!levels[2].mesh.isValid() && levels[2].errorMetres >= levels[1].errorMetres);
            assert(levels[2].errorMetres >= ForestUnderstoreySettings{}.impostorErrorMetres);

            // The card (scene/impostor.h): drawn only from the error the leaves end on, both chosen by the plant's one
            // box; a cut-out of its baked atlases, casting no shadow.
            const MeshLevelOptions& cardOptions = forest.assets.getMeshLevelOptions().at(card.parts.front().mesh.index);
            const MeshLevelOptions& leafOptions = forest.assets.getMeshLevelOptions().at(leaves.parts.front().mesh.index);
            assert(cardOptions.hiddenBelowError == levels[2].errorMetres && leafOptions.hiddenBelowError == 0.0f);
            assert(cardOptions.chooseBounds && leafOptions.chooseBounds && cardOptions.chooseBounds->maximum.y == leafOptions.chooseBounds->maximum.y);
            assert(card.parts.front().material.impostorFrames == ForestUnderstoreySettings{}.impostorFrames && !card.castsShadows);
            assert(isCutOut(card.parts.front().material));

            // Light comes through the plant's leaves and its card alike.
            assert(leaves.parts.front().material.diffuseTransmission == ForestLeafLightSettings{}.share);
            assert(card.parts.front().material.diffuseTransmission == leaves.parts.front().material.diffuseTransmission);
        }

        for (const int count : variants) {
            assert(count >= 1);
        }

        for (std::size_t plant = 0; plant < placed.size(); plant += 97) {
            for (const TreePlacement& tree : game.getTreePlacements()) {
                assert((Vec2{placed[plant].foot.x, placed[plant].foot.z} - Vec2{tree.foot.x, tree.foot.z}).length() >=
                       ForestUnderstoreySettings{}.trunkClearance);
            }
        }
    }
}

void testUnderstorey() {
    testTheUnderstoreyIsPlaced();
    testTheUnderstoreyThinsAwayFromThePath();
    testAPlantStandsOnItsFoot();
    testAKindsTallestPlantSetsItsScale();
    testTheUnderstoreySettings();
    testTheGamePlantsTheUnderstorey();
}
