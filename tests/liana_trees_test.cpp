#include "render/gpu/bone_poses.h"
#include "render/gpu/liana_trees.h"
#include "scene/planting_cells.h"

#include <cassert>
#include <cmath>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
    // Whether a span from its start, size long, holds a place as plantingCellBox's edges do (start in, end out).
    bool holds(float start, float size, float place) {
        return place >= start && place < start + size;
    }

    // Whether any cell holds a place: in floats a cell's end and the next one's start may differ by a rounding, leaving
    // a sliver no cell's writing pass plants in.
    bool heldByACell(float place, float size) {
        const int near = static_cast<int>(std::floor(place / size));

        for (int index = near - 1; index <= near + 1; ++index) {
            if (holds(static_cast<float>(index) * size, size, place)) {
                return true;
            }
        }

        return false;
    }

    /*
    * A foot's cell is one whose box (plantingCellBox, as the cells' writing pass takes plants by) holds it: west edge
    * in, east out, below 0 too, and where x / cellMetres rounds across an edge the box still decides.
    */
    void testTheCellOfAFoot() {
        const float cellMetres = 128.0f;
        assert((lianaTreeCell(10.0f, 200.0f, cellMetres) == PlantingCell{0, 1}));
        assert((lianaTreeCell(-0.5f, -128.0f, cellMetres) == PlantingCell{-1, -1}));
        assert((lianaTreeCell(256.0f, 255.99f, cellMetres) == PlantingCell{2, 1}));

        for (const float size : {128.0f, 64.0f, 37.3f, 0.7f}) {
            for (int step = -2000; step <= 2000; ++step) {
                // Points about every edge: on it, a float either side, and inside.
                const float edge = static_cast<float>(step / 4) * size;
                const int kind = ((step % 4) + 4) % 4;
                const float x = kind == 0 ? edge : kind == 1 ? std::nextafter(edge, -1e9f) : kind == 2 ? std::nextafter(edge, 1e9f) : edge + 0.37f * size;
                const PlantingCell cell = lianaTreeCell(x, -x, size);
                assert(!heldByACell(x, size) || holds(plantingCellBox(PlantingCell{cell.column, 0}, size).west, size, x));
                assert(!heldByACell(-x, size) || holds(plantingCellBox(PlantingCell{0, cell.row}, size).north, size, -x));
            }
        }
    }

    // A cell's place in a table side a side: column and row modulo side, whole below 0 too.
    void testACellsPlace() {
        assert(lianaCellPlace(PlantingCell{0, 0}, 13) == 0u);
        assert(lianaCellPlace(PlantingCell{2, 1}, 13) == 15u);
        assert(lianaCellPlace(PlantingCell{-1, -1}, 13) == 12u + 12u * 13u);
        assert(lianaCellPlace(PlantingCell{-27, 40}, 13) == 12u + 1u * 13u);
    }

    // Feet within a centimetre are one tree; further apart, two.
    void testTheSameTree() {
        assert(lianaSameTree(Vec3{1.0f, 2.0f, 3.0f}, Vec3{1.0f, 2.0f, 3.0f}));
        assert(lianaSameTree(Vec3{1.0f, 2.0f, 3.0f}, Vec3{1.005f, 2.0f, 3.005f}));
        assert(!lianaSameTree(Vec3{1.0f, 2.0f, 3.0f}, Vec3{1.0f, 2.02f, 3.0f}));
    }

    // Two sets as the GPU's cells hold them: the coarse one writing field layers 0 and 1 of its three (4 only read), the fine one layer 3.
    struct TwoSets {
        PlantingCellSetSettings coarse{.cellMetres = 128.0f, .radiusMetres = 400.0f, .cellsAFrame = 1000};
        PlantingCellSetSettings fine{.cellMetres = 32.0f, .radiusMetres = 100.0f, .cellsAFrame = 1000};
        PlantingCellRing coarseRing;
        PlantingCellRing fineRing;
        PlantingSlotLayout coarseLayout;
        PlantingSlotLayout fineLayout;
        std::vector<int> coarseLayers{0, 1, 4};
        std::vector<int> fineLayers{3};

        TwoSets() {
            coarseLayout.layerFirst = {0, 40, 140};
            coarseLayout.layerCapacity = {40, 100, 0};
            coarseLayout.written = {true, true, false};
            coarseLayout.plantsASlot = 140;
            fineLayout.layerFirst = {0};
            fineLayout.layerCapacity = {500};
            fineLayout.written = {true};
            fineLayout.plantsASlot = 500;
        }

        std::uint32_t coarseSlots() const {
            return static_cast<std::uint32_t>(plantingSlotCount(coarse));
        }

        std::vector<PlantingSetSlots> slots() const {
            const PlantingSetPlaces finePlaces{.plantBase = 140u * coarseSlots(), .countBase = 3u * coarseSlots()};
            return {PlantingSetSlots{coarseRing.getHeld(), coarseLayout, coarseLayers, PlantingSetPlaces{}, coarse.cellMetres,
                                     static_cast<std::uint32_t>(plantingSlotSide(coarse))},
                    PlantingSetSlots{fineRing.getHeld(), fineLayout, fineLayers, finePlaces, fine.cellMetres, static_cast<std::uint32_t>(plantingSlotSide(fine))}};
        }
    };

    /*
    * The tables: every held cell of each set at its place with its slot (never two at one place, wherever the camera
    * walks), the rest empty; each written layer's firsts, room and its set's table; the read-only and unplanted empty.
    */
    void testTheTables() {
        TwoSets sets;

        for (int step = 0; step < 40; ++step) {
            const Vec2 camera{static_cast<float>(step) * 97.3f - 1500.0f, static_cast<float>(step * step) * 3.1f - 900.0f};
            sets.coarseRing.update(camera, sets.coarse);
            sets.fineRing.update(camera, sets.fine);
            const LianaTreeTables tables = packLianaTreeTables(sets.slots(), 6);
            const std::uint32_t coarseSide = static_cast<std::uint32_t>(plantingSlotSide(sets.coarse));
            const std::uint32_t fineSide = static_cast<std::uint32_t>(plantingSlotSide(sets.fine));
            assert(coarseSide * coarseSide == sets.coarseSlots());
            assert(tables.cells.size() == coarseSide * coarseSide + fineSide * fineSide);
            std::size_t filled = 0;

            for (const GpuLianaCell& cell : tables.cells) {
                filled += cell.slot != lianaCellEmpty ? 1 : 0;
            }

            assert(filled == sets.coarseRing.getHeld().size() + sets.fineRing.getHeld().size() && filled > 0);

            for (const auto& [cell, slot] : sets.fineRing.getHeld()) {
                const GpuLianaCell& placed = tables.cells[coarseSide * coarseSide + lianaCellPlace(cell, fineSide)];
                assert(placed.column == cell.column && placed.row == cell.row && placed.slot == static_cast<std::uint32_t>(slot));
            }

            assert(tables.layers.size() == 6);
            const GpuLianaTreeLayer& second = tables.layers[1];
            assert(second.firstPlant == 40u && second.plantsASlot == 140u && second.firstCount == 1u && second.countsASlot == 3u);
            assert(second.cellMetres == 128.0f && second.firstCell == 0u && second.side == coarseSide);
            const GpuLianaTreeLayer& fine = tables.layers[3];
            assert(fine.firstPlant == 140u * sets.coarseSlots() && fine.firstCount == 3u * sets.coarseSlots() && fine.countsASlot == 1u);
            assert(fine.firstCell == coarseSide * coarseSide && fine.side == fineSide && fine.cellMetres == 32.0f);
            assert(tables.layers[2].cellMetres == 0.0f && tables.layers[4].cellMetres == 0.0f && tables.layers[5].cellMetres == 0.0f);
        }

        // A written layer past the field's is refused (the fine set's layer 3 past 3 layers).
        bool threw = false;

        try {
            packLianaTreeTables(sets.slots(), 3);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
    }

    // A liana hung from a tree at foot in field layer 1 (its first hold), its far end a rope's in the ground.
    LianaShape lianaFrom(const Vec3& foot) {
        const LianaTree tree{.foot = foot, .height = 30.0f, .scale = 1.0f, .layer = 1, .binding = SwayBinding{.bone = 4, .weight = 1.0f}};
        return makeLianaShape(foot + Vec3{2.0f, 25.0f, 0.0f}, foot + Vec3{6.0f, 0.0f, 1.0f}, 30.0f, 0.02f, 2.0f, tree, nullptr, 0.3f);
    }

    // A placed plant standing at a foot.
    GpuPlacedPlant plantAt(const Vec3& foot) {
        return GpuPlacedPlant{{foot.x, foot.y, foot.z}, 1.0f, 0.0f, 0.5f, 0u, 0.0f};
    }

    /*
    * An anchor finds its tree among its cell's plants of its layer (the slot the cell is held in, not the first); a
    * rope's end, a foot no plant stands on, one past the reach, in a cell not held or past its slot's count finds none.
    */
    void testAnAnchorFindsItsTree() {
        TwoSets sets;
        sets.coarseRing.update(Vec2{0.0f, 0.0f}, sets.coarse);
        sets.fineRing.update(Vec2{0.0f, 0.0f}, sets.fine);
        const LianaTreeTables tables = packLianaTreeTables(sets.slots(), 6);
        const PlantingCell cell{1, -1};
        const std::uint32_t slot = static_cast<std::uint32_t>(sets.coarseRing.getHeld().at(cell));
        assert(slot != 0u);

        // Three trees of layer 1 in the cell's slot, and one of layer 0 at the third's foot.
        std::vector<GpuPlacedPlant> plants(140u * sets.coarseSlots());
        std::vector<std::uint32_t> counts(3u * sets.coarseSlots(), 0u);
        const Vec3 feet[3] = {Vec3{140.0f, 3.0f, -20.0f}, Vec3{170.5f, 4.0f, -90.0f}, Vec3{250.0f, 2.0f, -1.0f}};

        for (std::uint32_t tree = 0; tree < 3; ++tree) {
            plants[slot * 140u + 40u + tree] = plantAt(feet[tree]);
        }

        plants[slot * 140u] = plantAt(feet[2]);
        counts[slot * 3u + 1u] = 3u;
        counts[slot * 3u] = 1u;
        const Vec3 eye{150.0f, 10.0f, -50.0f};

        for (std::uint32_t tree = 0; tree < 3; ++tree) {
            assert(lianaTreePlant(lianaFrom(feet[tree]), 0, tables, plants, counts, eye, 300.0f) == slot * 140u + 40u + tree);
        }

        assert(lianaTreePlant(lianaFrom(feet[0]), 1, tables, plants, counts, eye, 300.0f) == BONE_NO_TREE);
        assert(lianaTreePlant(lianaFrom(Vec3{141.0f, 3.0f, -20.0f}), 0, tables, plants, counts, eye, 300.0f) == BONE_NO_TREE);
        assert(lianaTreePlant(lianaFrom(feet[0]), 0, tables, plants, counts, eye, 5.0f) == BONE_NO_TREE);
        const Vec3 far{9000.0f, 3.0f, -20.0f};
        assert(lianaTreePlant(lianaFrom(far), 0, tables, plants, counts, far, 300.0f) == BONE_NO_TREE);

        // A tree past its slot's count is not there.
        counts[slot * 3u + 1u] = 2u;
        assert(lianaTreePlant(lianaFrom(feet[2]), 0, tables, plants, counts, eye, 300.0f) == BONE_NO_TREE);
    }
}

void testLianaTrees() {
    testTheCellOfAFoot();
    testACellsPlace();
    testTheSameTree();
    testTheTables();
    testAnAnchorFindsItsTree();
}
