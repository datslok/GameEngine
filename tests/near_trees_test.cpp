#include "games/forest/near_trees.h"
#include "games/forest/forest_planting.h"
#include "scene/planting_cells.h"

#include "forest_helpers.h"
#include "test_jobs.h"

#include <algorithm>
#include <array>
#include <cassert>
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

    // The near trees' settings: the cells' size, start-up only.
    void testNearTreeSettings() {
        ForestNearTreeSettings settings;
        FieldCounter counter;
        visit(settings, counter);
        assert(counter.fields == 1 && counter.live == 0 && settings.cellMetres == 64.0f);
    }
}

void testNearTrees() {
    testCellsOverABox();
    testBigTreesAreTheForests();
    testLianasDoNotDependOnTheCells();
    testNearTreeSettings();
}
