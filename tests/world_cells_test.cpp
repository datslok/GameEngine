#include "engine/debug_views.h"
#include "render/render_stats.h"
#include "scene/world_cells.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    // Cells count from the origin, floored: the edge belongs to the cell east (south) of it, negatives count down, and
    // points far past a float's metre still land in their cell.
    void testCellKeys() {
        assert((worldCellOf(Vec3{0.0f, 0.0f, 0.0f}, 128.0f) == CellKey{0, 0}));
        assert((worldCellOf(Vec3{127.9f, 50.0f, 1.0f}, 128.0f) == CellKey{0, 0}));
        assert((worldCellOf(Vec3{128.0f, 0.0f, 0.0f}, 128.0f) == CellKey{1, 0}));
        assert((worldCellOf(Vec3{-0.1f, 0.0f, -0.1f}, 128.0f) == CellKey{-1, -1}));
        assert((worldCellOf(Vec3{-128.0f, 0.0f, 0.0f}, 128.0f) == CellKey{-1, 0}));
        assert((worldCellOf(Vec3{-128.5f, 0.0f, 0.0f}, 128.0f) == CellKey{-2, 0}));
        assert((worldCellOf(Vec3{0.0f, 0.0f, 1.0e7f}, 128.0f) == CellKey{0, 78125}));
        assert((worldCellOf(Vec3{4.5e12f, 0.0f, 0.0f}, 128.0f).x > 30000000000LL));
        assert((worldCellOf(Vec3{10.0f, 0.0f, 10.0f}, 4.0f) == CellKey{2, 2}));

        // Ordered by column, then row: the order a full layout walks them in.
        assert((CellKey{0, 5} < CellKey{1, -5}) && (CellKey{1, -5} < CellKey{1, 0}));
    }

    // A block holds the draws and their spare share, never fewer than the fewest.
    // A block holds the draws and their spare share, never fewer than the fewest, in whole chunks (so chunk = slot / 64).
    void testBlockSlots() {
        const CellSettings settings;
        assert(settings.chunkSlots == 64);
        assert(cellBlockSlots(0, settings) == 64);
        assert(cellBlockSlots(10, settings) == 64);
        assert(cellBlockSlots(100, settings) == 128);
        assert(cellBlockSlots(1000, settings) == 1280);

        CellSettings tight;
        tight.spareShare = 0.0f;
        tight.fewestSlots = 1;
        tight.chunkSlots = 1;
        assert(cellBlockSlots(7, tight) == 7);
        assert(cellBlockSlots(0, tight) == 1);

        tight.chunkSlots = 8;
        assert(cellBlockSlots(7, tight) == 8);
        assert(cellBlockSlots(9, tight) == 16);
    }

    /*
    * Morton order inside a cell: the place's x and z within the cell, 16 bits each, bits interleaved (x's in the even
    * places), so places near each other get numbers near each other and runs of slots stay close together on the ground.
    */
    void testMortonCodes() {
        assert(interleaveBits(0, 0) == 0);
        assert(interleaveBits(1, 0) == 1);
        assert(interleaveBits(0, 1) == 2);
        assert(interleaveBits(3, 3) == 15);
        assert(interleaveBits(0xFFFF, 0) == 0x55555555u);
        assert(interleaveBits(0, 0xFFFF) == 0xAAAAAAAAu);

        const CellKey cell{2, -1};
        assert(cellMortonCode(Vec3{256.0f, 0.0f, -128.0f}, cell, 128.0f) == 0);
        assert(cellMortonCode(Vec3{383.99f, 50.0f, -0.01f}, cell, 128.0f) > 0xFFF00000u);

        // Points outside the cell are held to its edges.
        assert(cellMortonCode(Vec3{0.0f, 0.0f, -200.0f}, cell, 128.0f) == 0);
        assert(cellMortonCode(Vec3{500.0f, 0.0f, 10.0f}, cell, 128.0f) == 0xFFFFFFFFu);

        // The quarter nearer the origin first: the cell's four quarters in Z order.
        const std::uint32_t nearCorner = cellMortonCode(Vec3{260.0f, 0.0f, -120.0f}, cell, 128.0f);
        const std::uint32_t eastQuarter = cellMortonCode(Vec3{330.0f, 0.0f, -120.0f}, cell, 128.0f);
        const std::uint32_t southQuarter = cellMortonCode(Vec3{260.0f, 0.0f, -50.0f}, cell, 128.0f);
        const std::uint32_t farQuarter = cellMortonCode(Vec3{330.0f, 0.0f, -50.0f}, cell, 128.0f);
        assert(nearCorner < eastQuarter && eastQuarter < southQuarter && southQuarter < farQuarter);
    }

    // Runs taken first fit, lowest first, else at the end; given back runs join neighbours; free runs at the end leave it.
    void testSlotBlocks() {
        SlotBlocks blocks;
        assert(blocks.end() == 0);
        assert(blocks.take(10) == 0);
        assert(blocks.take(5) == 10);
        assert(blocks.end() == 15);

        blocks.giveBack(0, 10);
        assert(blocks.end() == 15);
        assert(blocks.take(4) == 0);
        assert(blocks.take(6) == 4);
        assert(blocks.take(1) == 15);
        assert(blocks.end() == 16);

        // [10, 15) then [15, 16) back: joined, and at the end, so the end falls to 10.
        blocks.giveBack(10, 5);
        assert(blocks.end() == 16);
        blocks.giveBack(15, 1);
        assert(blocks.end() == 10);

        // Too big for any free run: at the end.
        blocks.giveBack(0, 4);
        assert(blocks.take(5) == 10);
        assert(blocks.take(4) == 0);

        bool threw = false;

        try {
            blocks.take(0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
        threw = false;

        try {
            blocks.giveBack(14, 2);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
        blocks.clear();
        assert(blocks.end() == 0 && blocks.take(3) == 0);
    }
}

namespace {
    // F4's line (and the stats command): the cells holding still draws, their slots, and the kilobytes the frame copied
    // into GPU memory kept between frames.
    void testTheReadoutShowsCells() {
        RenderStats stats;
        stats.stillCells = 12;
        stats.stillSlots = 50000;
        stats.lastingCopiedBytes = 3 * 1024 + 100;
        const std::vector<std::string> lines = renderStatsLines(stats);
        assert(std::find(lines.begin(), lines.end(), "CELLS 12  SLOTS 50000  UPLOADED 3 KB") != lines.end());
    }
}

void testWorldCells() {
    testTheReadoutShowsCells();
    testCellKeys();
    testBlockSlots();
    testMortonCodes();
    testSlotBlocks();
}
