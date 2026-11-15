#pragma once

#include "core/job_system.h"
#include "ecs/world.h"
#include "scene/frame_description.h"

#include <cstddef>
#include <cstdint>
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
* The still entities' draws kept from frame to frame (docs/PERSISTENTSCENE.md, stage 1), so a frame builds only what
* moves: the 35,000 objects of the forest cost the CPU 8.5 ms a frame to build and attach, almost all of them still.
* buildFrame takes the kept draws, puts them first (FrameDescription::stillDraws) and builds the moving ones after;
* once the frame is drawn the engine gives the list back (giveBack), and the next frame starts from it.
*
* Built again, all of them, when a still entity is added, destroyed, hidden or shown, given more or fewer parts, or no
* longer still, when a frame was not given back, or after invalidate; in place, an entity's own, when it is marked
* (RenderChanged). A deforming mesh's entity is never kept: its shape changes every frame. Kept draws stay in their
* order while kept; a full build lays them out in the renderers' order. Render-only: nothing here touches the
* simulation.
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
    // it changes nothing), built on jobs. Each one's last-frame matrix is its own.
    Update update(const World& world, float alpha, JobSystem& jobs);

    // The frame's list back once drawn: its first draws are kept for the next frame, the rest dropped then.
    void giveBack(std::vector<DrawItem> draws);

    // Every still draw is built again next frame: what draws hold from elsewhere (the assets' levels) may have changed.
    void invalidate();

    /*
    * The debug builds' check, up to count kept entities a call, taking turns: throws std::logic_error when one's draws
    * no longer match where it stands (moved without markRenderChanged). Nothing while the list is out.
    */
    void verify(const World& world, std::size_t count);

    std::size_t getCount() const;

private:
    // A kept entity, by its index: its generation, its first draw and its parts.
    struct Entry {
        std::uint32_t generation = 0;
        std::uint32_t first = 0;
        std::uint32_t parts = 0;
        bool present = false;
    };

    void buildAll(const World& world, const std::vector<std::uint8_t>& kept, float alpha, JobSystem& jobs);

    std::vector<Entry> byIndex;
    std::vector<Entity> order; // the kept entities, in their draws' order
    std::vector<DrawItem> draws;
    std::size_t count = 0;
    bool holding = false; // the list is here (given back since the last update)
    bool valid = false;   // what it holds is a whole build, not let go
    std::size_t verifyNext = 0;
    std::uint64_t version = 0;
};
