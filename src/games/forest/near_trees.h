#pragma once

#include "core/job_system.h"
#include "core/settings_visitor.h"
#include "games/forest/forest_planting.h"
#include "games/forest/forest_trees.h"
#include "ecs/world.h"
#include "games/forest/lianas.h"
#include "scene/liana_shape.h"
#include "scene/material.h"
#include "scene/wind.h"
#include "scene/planting_cells.h"
#include "scene/planting_maps.h"
#include "scene/terrain_heights.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

/*
* The big trees the CPU keeps near the player (ROADMAP phase 18, d2; docs/architecture/forest-trees.md): planted by the
* same maths as the GPU's cells (plantLayers), a square cell of land at a time, with the lianas hung from their crowns.
* A cell's trees and lianas are the same whichever cells are made with it, so cells come and go one by one.
*/

/*
* The near cells (F5 Forest, Near trees): their side (the lianas do not depend on it), those within radiusMetres of the
* player held (about 200 m: a liana a few centimetres across is a third of a pixel there), at most cellsAFrame made a
* frame, nearest first.
*/
struct ForestNearTreeSettings {
    float cellMetres = 64.0f;
    float radiusMetres = 200.0f;
    int cellsAFrame = 2;
};

void visit(ForestNearTreeSettings& settings, SettingsVisitor& visitor);

// A big tree (emergent or canopy) and the dither cell of its layer it grew from, which name it.
struct NearTree {
    TreePlacement placement;
    ForestLayer layer = ForestLayer::Canopy;
    int column = 0;
    int row = 0;
};

// A tree's identity: its layer and dither cell, the same whichever box planted it.
std::uint64_t nearTreeIdentity(const NearTree& tree);

// The big trees standing in a box: the emergents, then the canopy trees, each layer by rows from the north (plantLayers).
std::vector<NearTree> plantBigTrees(const ForestSpecies& species, const PlantingMaps& maps, const TerrainHeights& heights, const PlantingBox& box,
                                    JobSystem& jobs);

// The cells a box touches, by rows from the north then columns.
std::vector<PlantingCell> nearCellsOver(const PlantingBox& box, float cellMetres);

// What a near cell is made from: the species, maps and land the trees stand on, the region drawn (none: everywhere),
// and how the lianas hang.
struct NearTreeSources {
    const ForestSpecies& species;
    const PlantingMaps& maps;
    const TerrainHeights& heights;
    std::optional<PlantedRegion> region;
    const ForestLianaSettings& lianas;
    std::uint64_t lianaSeed = 0;
};

/*
* A cell's trees and lianas: the big trees inside the region whose feet the cell holds, and those within the lianas'
* neighbourSpan of it they may loop into, a crown each (trees[i] is crowns[i]'s tree; carries marks the cell's own),
* and the lianas the cell's own crowns hang (chooseLianas), their crowns indexing crowns.
*/
struct NearTreeCell {
    std::vector<NearTree> trees;
    std::vector<LianaCrown> crowns;
    std::vector<Liana> lianas;
};

NearTreeCell makeNearTreeCell(const PlantingCell& cell, float cellMetres, const NearTreeSources& sources, JobSystem& jobs);

// How the near lianas are drawn: the template tube (its levels named), the bark, the sway, the cascades they cast into;
// the ivy's templates (one a size of lianaLeafTemplateCards) and its leaf, a cut-out.
struct NearLianaLook {
    MeshHandle tube;
    Material bark;
    WindSway sway{};
    int shadowCascades = 0;
    std::array<MeshHandle, lianaLeafTemplateCards.size()> ivy{};
    Material leaf;
};

// What a liana's records take from the trees: each tree layer's variants' heights (their meshes' units), and the bend.
struct NearLianaTrees {
    const std::vector<float>& emergentHeights;
    const std::vector<float>& canopyHeights;
    float bend = 1.0f;
};

/*
* A cell's lianas as records (makeLianaShape): from each liana's trees, its far end fixed for a rope, with its ivy
* (lianaIvy) as the settings give it; at most most.
*/
std::vector<LianaShape> nearLianaShapes(const NearTreeCell& cell, const NearLianaTrees& trees, const ForestLianaSettings& settings, float barkTileMetres,
                                        std::size_t most);

/*
* The lianas round the player the GPU draws (ROADMAP phase 18, d2b, d2c): the near cells held round a place (the GPU
* cells' ring, scene/planting_cells.h), each cell's lianas a block of records in its slot (LianaShapes) and two still
* entities each naming its record: the template tube placed round it (lianaPose), then its ivy, the smallest template
* holding its cards (lianaLeafPose). A dropped cell's entities are destroyed and its block emptied; a cell made fills
* its slot's block anew. The cost grows with the cells made, not with those held.
*/
class NearLianas {
public:
    // The most lianas a cell's slot holds (a 64 m cell of the patch's densest forest hangs about 100).
    static constexpr std::size_t lianasASlot = 256;

    /*
    * The cells round place: those past the radius dropped, then up to most new ones made (nearest first). The records'
    * component, on holder, given anew when anything changed. True when anything did.
    */
    bool update(World& world, Entity holder, const Vec2& place, const ForestNearTreeSettings& settings, int most, const NearTreeSources& sources,
                const NearLianaTrees& trees, const NearLianaLook& look, float barkTileMetres, JobSystem& jobs);

    // Every liana's sway given anew, and every record's bend (each block replaced), the component given anew.
    void resway(World& world, Entity holder, const WindSway& sway, float bend);

    // The lianas' entities, cell by cell in their slots' order, and the cells held.
    const std::vector<Entity>& getEntities() const;
    std::size_t getCellCount() const;
    const LianaShapes& getShapes() const;

private:
    struct Slot {
        bool held = false;
        PlantingCell cell;
        std::vector<Entity> entities;
    };

    void dropSlot(World& world, std::size_t slot);
    void gatherEntities();

    PlantingCellRing ring;
    std::vector<Slot> slots;
    LianaShapes shapes;
    std::vector<Entity> entities;
};
