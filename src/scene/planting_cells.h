#pragma once

#include "core/settings_visitor.h"
#include "math/vec2.h"
#include "scene/model_renderer.h"
#include "scene/planting_maps.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

/*
* The cells round the camera a stretch of land is planted in on the GPU (ROADMAP phase 18, the valley's stages 2b and
* e1, docs/architecture/planting.md): sets of cells of their own sizes and radii, each planting the layers naming it;
* which square cells are held, which are made this frame and into which fixed slot, and how a slot holds each layer's
* plants. Each cell is planted alone (plantLayers' box alone equals the whole), so cells come and go one by one.
*/

// The maps and layers a game plants from, as the renderer's cells read them (PlantingSource names them in the world).
struct PlantingField {
    PlantingMaps maps;
    std::vector<PlantingLayerRecord> layers;
};

// How one kind of a layer's plants is drawn on the GPU: its models in the plant's own frame (the plant's foot, turn and
// scale place them, as an entity's Transform would). A kind given several looks (variants 0 to n - 1) draws the one
// its plant's pick chooses, min(pick * n, n - 1) (placedKindSlot).
struct PlantedKindLook {
    int layer = 0;
    int kind = 0;
    int variant = 0;
    std::vector<ModelRenderer> models;
};

/*
* How a layer's plants are drawn beyond their kinds' looks: groundRelief, the most metres the land under any of its
* plants lies above or below its foot, by which its ground-following parts' culling spheres grow (their vertices lifted
* onto the land: terrain_data.glsl's groundLift).
*/
struct PlantedLayerDrawing {
    float groundRelief = 0.0f;
};

// The kinds drawn from the cells, wherever planted, and how each layer is drawn (by layer; none past the end: no relief).
struct PlantingLooks {
    std::vector<PlantedKindLook> kinds;
    std::vector<PlantedLayerDrawing> layers;
};

/*
* A world's planting field: the first one placed is the frame's (FrameDescription::planting). version changes when the
* field is rebuilt, so the renderer uploads it again. looks, when there, draws its plants (read every frame: a game
* shows or hides them by setting it).
*/
struct PlantingSource {
    std::shared_ptr<const PlantingField> field;
    std::uint64_t version = 0;
    std::shared_ptr<const PlantingLooks> looks;
};

/*
* One set of cells: each a square of cellMetres from the world's origin; those within radius of the camera held, at
* most cellsAFrame made a frame, nearest first; a held one dropped once it is a cell farther than radius.
*/
struct PlantingCellSetSettings {
    float cellMetres = 64.0f;
    float radiusMetres = 300.0f;
    int cellsAFrame = 4;

    bool operator==(const PlantingCellSetSettings&) const = default;
};

void visit(PlantingCellSetSettings& settings, SettingsVisitor& visitor);

/*
* The cells (F5 Planting): off by default, for comparing (placement-compare) while nothing draws them; on by themselves
* while a field's looks draw some. A set each of plantingCoarseCells to plantingFineCells (Horizon's: trees in 128 m
* blocks, grass in 32 m): the coarse 128 m cells to 1 km, the middle 64 m to 300 m, the fine 32 m to 100 m. Each set
* plants the layers naming it (PlantingLayerRecord::cellSet), so a layer is planted only as far as it is drawn.
*/
struct PlantingCellSettings {
    bool enabled = false;
    std::array<PlantingCellSetSettings, plantingCellSetCount> sets{
        PlantingCellSetSettings{.cellMetres = 128.0f, .radiusMetres = 1000.0f, .cellsAFrame = 4},
        PlantingCellSetSettings{.cellMetres = 64.0f, .radiusMetres = 300.0f, .cellsAFrame = 4},
        PlantingCellSetSettings{.cellMetres = 32.0f, .radiusMetres = 100.0f, .cellsAFrame = 4},
    };
};

void visit(PlantingCellSettings& settings, SettingsVisitor& visitor);

/*
* What one set of cells plants: the layers naming it and every layer they read (keepAway, under, and what those read),
* in the field's order, renumbered among themselves (their under and keep-aways too); fieldLayer, each one's number in
* the field; written, whether it names the set (the rest are planted only to be read, never kept in a slot). A set no
* layer names plants nothing. Throws std::invalid_argument for a layer naming no set.
*/
struct PlantingSetLayers {
    std::vector<PlantingLayerRecord> layers;
    std::vector<int> fieldLayer;
    std::vector<bool> written;
};

PlantingSetLayers plantingSetLayers(const std::vector<PlantingLayerRecord>& layers, int set);

// One cell: x from column * cellMetres, z from row * cellMetres, a cell's side each way.
struct PlantingCell {
    int column = 0;
    int row = 0;

    auto operator<=>(const PlantingCell&) const = default;
};

PlantingBox plantingCellBox(const PlantingCell& cell, float cellMetres);

// The most cells held at once: those touching a square of the drop distance either way round any place. Its side: the
// most held across or down, so no two held cells share their columns and rows modulo it.
int plantingSlotCount(const PlantingCellSetSettings& settings);
int plantingSlotSide(const PlantingCellSetSettings& settings);

/*
* Where a slot keeps each layer's plants (layerFirst, at most layerCapacity: the lattice cells over a cell's box, each
* at most one plant; none for a layer not written) and where a cell being made keeps each layer's candidates
* (scratchFirst, scratchCapacity: the lattice over the layer's candidates box), counted in plants and candidates.
*/
struct PlantingSlotLayout {
    std::vector<std::uint32_t> layerFirst;
    std::vector<std::uint32_t> layerCapacity;
    std::vector<bool> written;
    std::uint32_t plantsASlot = 0;
    std::vector<std::uint32_t> scratchFirst;
    std::vector<std::uint32_t> scratchCapacity;
    std::uint32_t candidatesACell = 0;
};

// written: a flag a layer (empty: every layer written).
PlantingSlotLayout plantingSlotLayout(const std::vector<PlantingLayerRecord>& layers, float cellMetres, const std::vector<bool>& written = {});

// A cell to make this frame and the slot it goes into.
struct PlantingCellMade {
    PlantingCell cell;
    int slot = 0;
};

/*
* The cells held round the camera and their slots. update() drops those past the drop distance (their slots free), then
* names the cells within radius not yet held, nearest first (by the nearest point of each, then by row and column), up
* to cellsAFrame, each into the lowest free slot, held from then on. A change of the cells' size or radius, or reset(),
* forgets them all. Ordered by cell, never by address, so the same moves make the same cells in the same slots.
*/
class PlantingCellRing {
public:
    std::vector<PlantingCellMade> update(const Vec2& camera, const PlantingCellSetSettings& settings);
    void reset();

    const std::map<PlantingCell, int>& getHeld() const;
    int getSlotCount() const;

private:
    float cellMetres = 0.0f;
    float radiusMetres = 0.0f;
    std::map<PlantingCell, int> held;
    std::vector<bool> slotTaken;
};
