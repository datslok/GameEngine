#include "games/forest/near_trees.h"
#include "games/forest/forest_planting.h"
#include "scene/model_renderer.h"
#include "scene/planting_cells.h"

#include "forest_helpers.h"
#include "test_jobs.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <set>
#include <tuple>
#include <vector>

namespace {
    // The patch's trees and lianas as the game makes them: the default species on the patch's maps and land. The
    // sources hold the liana settings by reference, so lianas must outlive them.
    NearTreeSources patchSources(const ForestLianaSettings& lianas) {
        return NearTreeSources{.species = defaultSpecies(),
                               .maps = patchValleyMaps().planting,
                               .heights = patchTerrainHeights(),
                               .region = forestPatch,
                               .lianas = lianas,
                               .lianaSeed = 11};
    }

    /*
    * A box's big trees are the forest's: the emergents then the canopy trees plantTheForest stands in it, in its order,
    * each named once by its layer and dither cell.
    */
    void testBigTreesAreTheForests() {
        const PlantingBox box = plantingCellBox(PlantingCell{0, -3}, 64.0f);
        const std::vector<NearTree> trees = plantBigTrees(defaultSpecies(), patchValleyMaps().planting, patchTerrainHeights(), box, testJobs());
        const ForestPlanting forest = plantTheForest(defaultSpecies(), patchValleyMaps(), patchTerrainHeights(), box, testJobs());
        assert(trees.size() == forest.emergents.size() + forest.canopy.size() && !forest.canopy.empty());
        std::set<std::uint64_t> identities;

        for (std::size_t index = 0; index < trees.size(); ++index) {
            const bool emergent = index < forest.emergents.size();
            const TreePlacement& expected = emergent ? forest.emergents[index] : forest.canopy[index - forest.emergents.size()];
            const NearTree& tree = trees[index];
            assert(tree.layer == (emergent ? ForestLayer::Emergent : ForestLayer::Canopy));
            assert((tree.placement.foot - expected.foot).length() == 0.0f && tree.placement.scale == expected.scale);
            assert(tree.placement.turnRadians == expected.turnRadians && tree.placement.variant == expected.variant);
            assert(tree.placement.foot.x >= box.west && tree.placement.foot.x < box.east);
            assert(tree.placement.foot.z >= box.north && tree.placement.foot.z < box.south);
            identities.insert(nearTreeIdentity(tree));
        }

        assert(identities.size() == trees.size());
    }

    // A liana as numbers to compare: its holds, length, thickness and seed.
    using LianaKey = std::tuple<float, float, float, float, float, float, float, float, std::uint32_t>;

    LianaKey keyOf(const Liana& liana) {
        return {liana.from.x, liana.from.y, liana.from.z, liana.to.x, liana.to.y, liana.to.z, liana.length, liana.radius, liana.seed};
    }

    // Every liana of the cells over the patch's box, cellMetres a side, checking each cell's own as it goes.
    std::vector<LianaKey> patchLianas(float cellMetres) {
        const ForestLianaSettings lianas;
        const NearTreeSources sources = patchSources(lianas);
        std::vector<LianaKey> keys;

        for (const PlantingCell& cell : nearCellsOver(regionBox(forestPatch), cellMetres)) {
            const NearTreeCell made = makeNearTreeCell(cell, cellMetres, sources, testJobs());
            const PlantingBox box = plantingCellBox(cell, cellMetres);
            assert(made.trees.size() == made.crowns.size());

            for (std::size_t crown = 0; crown < made.crowns.size(); ++crown) {
                const Vec3& foot = made.trees[crown].placement.foot;
                const bool mine = foot.x >= box.west && foot.x < box.east && foot.z >= box.north && foot.z < box.south;
                assert(made.crowns[crown].carries == mine && made.crowns[crown].identity == nearTreeIdentity(made.trees[crown]));
                assert(insidePlantedRegion(patchValleyMaps().planting, forestPatch, foot));
            }

            for (const Liana& liana : made.lianas) {
                assert(made.crowns[static_cast<std::size_t>(liana.crown)].carries);
                keys.push_back(keyOf(liana));
            }
        }

        std::sort(keys.begin(), keys.end());
        return keys;
    }

    /*
    * The lianas do not depend on how the land is cut into cells (ROADMAP phase 18, d2a): the patch's from 64 m cells
    * are exactly its from 48 m cells, each hung by the cell its crown's foot stands in, every tree in the patch.
    */
    void testLianasDoNotDependOnTheCells() {
        const std::vector<LianaKey> big = patchLianas(64.0f);
        const std::vector<LianaKey> small = patchLianas(48.0f);
        assert(!big.empty() && big == small);
    }

    // The cells over a box: every cell it touches, by rows from the north then columns, each once.
    void testCellsOverABox() {
        const std::vector<PlantingCell> cells = nearCellsOver(PlantingBox{.west = -10.0f, .east = 70.0f, .north = -64.0f, .south = 0.0f}, 64.0f);
        const std::vector<PlantingCell> expected{{-1, -1}, {0, -1}, {1, -1}};
        assert(cells == expected);
    }

    // The near trees' settings: the cells' size, their radius and how many a frame, all live.
    void testNearTreeSettings() {
        ForestNearTreeSettings settings;
        FieldCounter counter;
        visit(settings, counter);
        assert(counter.fields == 3 && counter.live == 3 && settings.cellMetres == 64.0f && settings.radiusMetres == 200.0f);
    }

    // A world holding near lianas, with the template and the trees' heights they are made with.
    struct LianaWorld {
        World world;
        AssetManager assets;
        Entity holder = world.create();
        NearLianas lianas;
        ForestLianaSettings lianaSettings;
        NearTreeSources sources = patchSources(lianaSettings);
        std::vector<float> heights{30.0f, 32.0f};
        NearLianaTrees trees{.emergentHeights = heights, .canopyHeights = heights, .bend = 0.25f};
        NearLianaLook look{.tube = assets.addDrawnMesh(lianaTemplateMesh(4, 3)), .bark = Material{}, .sway = WindSway{}, .shadowCascades = 2};
        ForestNearTreeSettings settings;

        bool update(const Vec2& place, int most) {
            return lianas.update(world, holder, place, settings, most, sources, trees, look, 2.0f, testJobs());
        }
    };

    // Each held liana's record, by the entity naming it, as numbers to compare.
    std::vector<LianaKey> heldLianas(const LianaWorld& held) {
        const LianaShapes& shapes = held.world.get<LianaShapes>(held.holder);
        std::vector<LianaKey> keys;

        for (const Entity entity : held.lianas.getEntities()) {
            const std::uint32_t record = held.world.get<ModelRenderer>(entity).lianaShape - 1u;
            const LianaShape& shape = (*shapes.slots[record / shapes.perSlot])[record % shapes.perSlot];
            keys.emplace_back(shape.from[0], shape.from[1], shape.from[2], shape.to[0], shape.to[1], shape.to[2], shape.to[3], shape.from[3], 0u);
        }

        std::sort(keys.begin(), keys.end());
        return keys;
    }

    /*
    * The lianas follow the place (ROADMAP phase 18, d2b): every cell within the radius made at once at the start, an
    * entity a liana naming its record in its cell's slot (still, the template, its look's cascades, its trees' bend);
    * moved away from the patch, every one dropped (its entities destroyed, its slot emptied); moved back, the same
    * lianas again. At most the asked cells a frame.
    */
    void testNearLianasFollowThePlace() {
        LianaWorld held;
        const Vec2 start{0.0f, -125.0f};
        assert(held.update(start, std::numeric_limits<int>::max()));
        assert(held.lianas.getCellCount() > 20 && !held.lianas.getEntities().empty());
        const std::vector<Entity> first = held.lianas.getEntities();
        const std::vector<LianaKey> lianas = heldLianas(held);
        std::size_t records = 0;

        for (const auto& block : held.world.get<LianaShapes>(held.holder).slots) {
            records += block ? block->size() : 0;
            assert(!block || block->size() <= NearLianas::lianasASlot);
        }

        assert(records == first.size());

        for (const Entity entity : first) {
            const ModelRenderer& renderer = held.world.get<ModelRenderer>(entity);
            assert(renderer.isStill() && renderer.shadowCascades == 2 && renderer.parts.front().mesh == held.look.tube);
        }

        assert(!held.update(start, std::numeric_limits<int>::max()));

        // Far from the patch: no trees drawn there, so no lianas, and every earlier one gone.
        assert(held.update(Vec2{0.0f, 3000.0f}, std::numeric_limits<int>::max()));
        assert(held.lianas.getEntities().empty());

        for (const Entity entity : first) {
            assert(!held.world.isAlive(entity));
        }

        held.update(start, std::numeric_limits<int>::max());
        assert(heldLianas(held) == lianas);

        // A frame's cells: at most the asked.
        LianaWorld fresh;
        fresh.update(start, 1);
        assert(fresh.lianas.getCellCount() == 1);
    }

    // The bend given anew reaches every record, each block replaced (so the renderer copies it again).
    void testNearLianasResway() {
        LianaWorld held;
        held.update(Vec2{0.0f, -125.0f}, std::numeric_limits<int>::max());
        const LianaShapes before = held.world.get<LianaShapes>(held.holder);
        held.lianas.resway(held.world, held.holder, WindSway{.kind = SwayKind::Hanging, .swing = 0.6f}, 0.4f);
        const LianaShapes& after = held.world.get<LianaShapes>(held.holder);

        for (std::size_t slot = 0; slot < after.slots.size(); ++slot) {
            if (after.slots[slot]) {
                assert(after.slots[slot] != before.slots[slot]);

                for (const LianaShape& shape : *after.slots[slot]) {
                    assert(shape.trees[2] == 0.4f);
                }
            }
        }

        assert(held.world.get<ModelRenderer>(held.lianas.getEntities().front()).sway.swing == 0.6f);
    }
}

void testNearTrees() {
    testCellsOverABox();
    testBigTreesAreTheForests();
    testLianasDoNotDependOnTheCells();
    testNearTreeSettings();
    testNearLianasFollowThePlace();
    testNearLianasResway();
}
