#pragma once

#include "core/job_system.h"
#include "ecs/world.h"
#include "scene/frame_description.h"
#include "scene/world_cells.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

/*
* Component, for one frame: a still entity (ModelRenderer::still) changed, so its draws are built again
* (markRenderChanged). The engine removes it once the frame is described (clearRenderChanged).
*/
struct RenderChanged {};

// Tells the engine a still entity's Transform, parts or materials changed: its draws are built again next frame. A
// still draw's last-frame matrix is always its own, so an announced move is a jump (no motion blur or smear for it).
// Harmless for an entity that is not still (built every frame anyway). Once per entity inside any World::each walk
// (the mark is added after the walk, and a second one would find it there).
void markRenderChanged(World& world, Entity entity);

// Removes every mark, once the frame that carries them is described (the engine does this every frame).
void clearRenderChanged(World& world);


/*
* A slot of the still draws no draw fills (a cell's spare room): no mesh, hidden, casting no shadow, bounds unknown, so
* every view, ray and list passes over it.
*/
DrawItem emptyDrawSlot();

// Whether a draw is one (no mesh: nothing else has none).
bool isEmptyDrawSlot(const DrawItem& draw);

/*
* The still entities' draws kept from frame to frame (docs/research/PERSISTENTSCENE.md, stage 1), so a frame builds only what
* moves: the 35,000 objects of the forest cost the CPU 8.5 ms a frame to build and attach, almost all of them still.
* buildFrame takes the kept draws, puts them first (FrameDescription::stillDraws) and builds the moving ones after;
* once the frame is drawn the engine gives the list back (giveBack), and the next frame starts from it.
*
* Laid out by cell (scene/world_cells.h, docs/research/GPUDRIVEN.md stage 7): each cell a block of slots with spare room
* (cellBlockSlots, whole chunks), cells by column then row, a cell's entities in Morton order (cellMortonCode, sorted
* again on every rebuild of the cell), its spare slots empty
* (emptyDrawSlot). A still entity added, destroyed, hidden or shown, given more or fewer parts, no longer still, or
* moved into another cell (announced) rebuilds the blocks of the cells it leaves and joins, nothing else; a cell that
* outgrows its block moves to the first free run that fits (SlotBlocks), its old block emptied. An announced change
* within its cell is built in place, its own draws. Everything is laid out afresh on the first frame, when a frame was
* not given back, after invalidate, or with other cell settings. The count of slots only grows between full layouts,
* and the built ranges cover every slot past the count before. A deforming mesh's entity is never kept: its shape
* changes every frame. Render-only: nothing here touches the simulation.
*/
class StillDraws {
public:
    // What a frame starts from: the still draws first (count of them, those in built built this frame), with every
    // renderer's place in the world's ModelRenderer storage marked 1 in kept when its draws are among them.
    struct Update {
        std::vector<DrawItem> draws;
        std::size_t count = 0;
        std::vector<DrawRange> built;
        std::vector<std::uint8_t> kept;
        std::uint64_t version = 0;         // this update's, never 0
        std::uint64_t previousVersion = 0; // the update before's (0: none)
    };

    // The still draws brought up to date with the world, placed at alpha (still entities have no PreviousTransform, so
    // it changes nothing), built on jobs. Each one's last-frame matrix is its own. Built ranges are sorted and apart.
    Update update(const World& world, float alpha, JobSystem& jobs);

    // The frame's list back once drawn: its first draws are kept for the next frame, the rest dropped then.
    void giveBack(std::vector<DrawItem> draws);

    // Every still draw is built again next frame: what draws hold from elsewhere (the assets' levels) may have changed.
    void invalidate();

    // The cells to lay the draws out by; other settings than the last lay everything out afresh next frame.
    void setCells(const CellSettings& settings);

    /*
    * The debug builds' check, up to count kept entities a call, taking turns: throws std::logic_error when one's draws
    * no longer match where it stands (moved without markRenderChanged). Nothing while the list is out.
    */
    void verify(const World& world, std::size_t count);

    // The slots (draws and empty ones) and the cells holding still draws.
    std::size_t getCount() const;
    std::size_t getCellCount() const;

    // The slots holding draws (the rest are empty), and the slots a chunk holds.
    std::size_t getFilledCount() const;
    std::size_t getChunkSlots() const;

private:
    // A kept entity, by its index: its generation, its cell, its first draw and its parts, and the update that last saw it.
    struct Entry {
        std::uint32_t generation = 0;
        std::uint32_t first = 0;
        std::uint32_t parts = 0;
        bool present = false;
        CellKey cell{};
        std::uint64_t seen = 0;
    };

    // A cell's block of slots and its entities, in their draws' order.
    struct Cell {
        std::size_t first = 0;
        std::size_t slots = 0;
        std::size_t used = 0; // slots its entities' draws fill
        std::vector<Entity> entities;
    };

    void buildAll(const World& world, const std::vector<std::uint8_t>& kept, float alpha, JobSystem& jobs);
    void buildCell(const World& world, const CellKey& key, Cell& cell, float alpha, JobSystem& jobs);
    void emptySlots(std::size_t first, std::size_t count);
    CellKey cellOfEntity(const World& world, Entity entity, float alpha) const;

    std::vector<Entry> byIndex;
    std::map<CellKey, Cell> cells; // by key: walked in a fixed order
    SlotBlocks blocks;
    CellSettings cellSettings;
    std::size_t keptEntities = 0;
    std::size_t filled = 0;
    std::vector<DrawItem> draws;
    std::size_t count = 0;
    bool holding = false; // the list is here (given back since the last update)
    bool valid = false;   // what it holds is a whole build, not let go
    std::size_t verifyNext = 0;
    std::uint64_t version = 0;
};
