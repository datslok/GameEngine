#include "games/forest/near_trees.h"
#include "games/forest/forest_terrain.h"
#include "scene/model_renderer.h"
#include "scene/still_draws.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>

void visit(ForestNearTreeSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Cell size", settings.cellMetres, {.min = 8.0f, .max = 512.0f, .sliderMax = 256.0f, .unit = "m"});
    visitor.field("Radius", settings.radiusMetres, {.min = 0.0f, .max = 2000.0f, .sliderMax = 600.0f, .unit = "m"});
    visitor.field("Cells a frame", settings.cellsAFrame, {.min = 1, .max = 64, .sliderMax = 16});
}

/*
* The layer and the dither cell's column and row packed into one number: column and row spread by an odd multiplier
* (so no two cells meet), the layer added, which a multiple of the multiplier can never cancel for the valley's cells.
*/
std::uint64_t nearTreeIdentity(const NearTree& tree) {
    const std::uint64_t cell = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tree.column)) << 32) | static_cast<std::uint32_t>(tree.row);
    return cell * 0x9E3779B97F4A7C15ull + static_cast<std::uint64_t>(tree.layer);
}

/*
* Only the emergents' and the canopy's layers: they read nothing after them, so planting the two alone stands the same
* trees as planting every layer (plantLayers' boxes), for a fraction of the work.
*/
std::vector<NearTree> plantBigTrees(const ForestSpecies& species, const PlantingMaps& maps, const TerrainHeights& heights, const PlantingBox& box,
                                    JobSystem& jobs) {
    std::vector<PlantingLayerRecord> layers = forestPlantingLayers(species);
    layers.resize(static_cast<std::size_t>(ForestLayer::Canopy) + 1);
    const std::vector<std::vector<PlantedPlant>> planted = plantLayers(layers, maps, heights, box, jobs);
    std::vector<NearTree> trees;

    for (const ForestLayer layer : {ForestLayer::Emergent, ForestLayer::Canopy}) {
        for (const PlantedPlant& plant : planted[static_cast<std::size_t>(layer)]) {
            trees.push_back(NearTree{TreePlacement{plant.foot, plant.scale, plant.turnRadians, plant.kind}, layer, plant.column, plant.row});
        }
    }

    return trees;
}

std::vector<PlantingCell> nearCellsOver(const PlantingBox& box, float cellMetres) {
    if (!(cellMetres > 0.0f)) {
        throw std::invalid_argument("nearCellsOver: a cell's side must be above 0");
    }

    // A box's end is out, so a box ending on a cell's edge does not touch the next.
    const int firstColumn = static_cast<int>(std::floor(box.west / cellMetres));
    const int lastColumn = static_cast<int>(std::ceil(box.east / cellMetres)) - 1;
    const int firstRow = static_cast<int>(std::floor(box.north / cellMetres));
    const int lastRow = static_cast<int>(std::ceil(box.south / cellMetres)) - 1;
    std::vector<PlantingCell> cells;

    for (int row = firstRow; row <= lastRow; ++row) {
        for (int column = firstColumn; column <= lastColumn; ++column) {
            cells.push_back(PlantingCell{column, row});
        }
    }

    return cells;
}

/*
* A crown's centre stands straight above its foot (placedCrownCentre), so every neighbour a cell's crowns may loop into
* has its foot within neighbourSpan of the cell: the cell grown by that, planted once. The trees keep plantBigTrees'
* order whatever box planted them, so each crown finds its neighbours in the same order (chooseLianas).
*/
NearTreeCell makeNearTreeCell(const PlantingCell& cell, float cellMetres, const NearTreeSources& sources, JobSystem& jobs) {
    const PlantingBox own = plantingCellBox(cell, cellMetres);
    const float margin = std::max(sources.lianas.neighbourSpan, 0.0f);
    const PlantingBox grown{.west = own.west - margin, .east = own.east + margin, .north = own.north - margin, .south = own.south + margin};
    NearTreeCell made;

    for (const NearTree& tree : plantBigTrees(sources.species, sources.maps, sources.heights, grown, jobs)) {
        const Vec3& foot = tree.placement.foot;
        const ForestTreeSettings& settings = tree.layer == ForestLayer::Emergent ? sources.species.emergent : sources.species.canopy;
        const bool mine = foot.x >= own.west && foot.x < own.east && foot.z >= own.north && foot.z < own.south;
        made.trees.push_back(tree);
        const CrownEnvelope envelope = treeCrown(settings);
        made.crowns.push_back(LianaCrown{.centre = placedCrownCentre(tree.placement, settings),
                                         .radius = envelope.radius * tree.placement.scale,
                                         .halfHeight = envelope.halfHeight * tree.placement.scale,
                                         .identity = nearTreeIdentity(tree),
                                         .carries = mine});
    }

    made.lianas = chooseLianas(made.crowns, sources.lianas, sources.lianaSeed, forestHeight, forestPathDistance);
    return made;
}

std::vector<LianaShape> nearLianaShapes(const NearTreeCell& cell, const NearLianaTrees& trees, const ForestLianaSettings& settings, float barkTileMetres,
                                        std::size_t most) {
    const auto treeOf = [&](int crown) {
        const NearTree& tree = cell.trees[static_cast<std::size_t>(crown)];
        const std::vector<float>& heights = tree.layer == ForestLayer::Emergent ? trees.emergentHeights : trees.canopyHeights;
        const std::size_t variant = static_cast<std::size_t>(tree.placement.variant);
        return LianaTree{tree.placement.foot, variant < heights.size() ? heights[variant] : 1.0f, tree.placement.scale};
    };
    std::vector<LianaShape> shapes;

    for (const Liana& liana : cell.lianas) {
        if (shapes.size() >= most) {
            break;
        }

        const LianaTree first = treeOf(liana.crown);
        const LianaTree second = liana.otherCrown >= 0 ? treeOf(liana.otherCrown) : LianaTree{};
        LianaShape shape = makeLianaShape(liana.from, liana.to, liana.length, liana.radius, barkTileMetres, first, liana.otherCrown >= 0 ? &second : nullptr,
                                          trees.bend);
        const LianaCrown* otherCrown = liana.otherCrown >= 0 ? &cell.crowns[static_cast<std::size_t>(liana.otherCrown)] : nullptr;
        setLianaLeaves(shape, lianaIvy(shape, cell.crowns[static_cast<std::size_t>(liana.crown)], otherCrown, settings, liana.seed));
        shapes.push_back(shape);
    }

    return shapes;
}

/*
* The ring names the cells made into their slots; a slot held for another cell, or no longer held, was dropped (the
* ring frees a slot before handing it out again, and forgets every cell when its settings change).
*/
bool NearLianas::update(World& world, Entity holder, const Vec2& place, const ForestNearTreeSettings& settings, int most,
                        const NearTreeSources& sources, const NearLianaTrees& trees, const NearLianaLook& look, float barkTileMetres, JobSystem& jobs) {
    const PlantingCellSetSettings ringSettings{.cellMetres = settings.cellMetres, .radiusMetres = settings.radiusMetres, .cellsAFrame = most};
    const std::vector<PlantingCellMade> made = ring.update(place, ringSettings);
    const std::map<PlantingCell, int>& held = ring.getHeld();
    bool changed = false;

    if (slots.size() != static_cast<std::size_t>(ring.getSlotCount())) {
        for (std::size_t slot = 0; slot < slots.size(); ++slot) {
            dropSlot(world, slot);
        }

        slots.assign(static_cast<std::size_t>(ring.getSlotCount()), Slot{});
        shapes.slots.assign(slots.size(), nullptr);
        shapes.perSlot = static_cast<std::uint32_t>(lianasASlot);
        changed = true;
    }

    for (std::size_t slot = 0; slot < slots.size(); ++slot) {
        const auto found = held.find(slots[slot].cell);

        if (slots[slot].held && (found == held.end() || found->second != static_cast<int>(slot))) {
            dropSlot(world, slot);
            changed = true;
        }
    }

    for (const PlantingCellMade& cell : made) {
        const std::size_t slot = static_cast<std::size_t>(cell.slot);
        dropSlot(world, slot);
        const NearTreeCell planted = makeNearTreeCell(cell.cell, settings.cellMetres, sources, jobs);
        auto block = std::make_shared<std::vector<LianaShape>>(nearLianaShapes(planted, trees, sources.lianas, barkTileMetres, lianasASlot));
        Slot& filling = slots[slot];
        filling.held = true;
        filling.cell = cell.cell;

        const auto addDraw = [&](MeshHandle mesh, const Material& material, std::uint32_t named, const Transform& pose) {
            ModelRenderer renderer = makeMeshRenderer(mesh, material).withStill(true);
            renderer.shadowCascades = look.shadowCascades;
            renderer.sway = look.sway;
            renderer.lianaShape = named;
            const Entity entity = world.create();
            world.add(entity, pose);
            world.add(entity, std::move(renderer));
            filling.entities.push_back(entity);
        };

        for (std::size_t index = 0; index < block->size(); ++index) {
            const LianaShape& shape = (*block)[index];
            const std::uint32_t named = static_cast<std::uint32_t>(slot * lianasASlot + index + 1);
            addDraw(look.tube, look.bark, named, lianaPose(shape));
            const float leaves = lianaLeafCount(shape);

            if (leaves > 0.0f) {
                addDraw(look.ivy[lianaLeafTemplate(leaves)], look.leaf, named | LIANA_IVY_BIT, lianaLeafPose(shape));
            }
        }

        shapes.slots[slot] = std::move(block);
        changed = true;
    }

    if (changed) {
        gatherEntities();

        if (world.has<LianaShapes>(holder)) {
            world.get<LianaShapes>(holder) = shapes;
        } else {
            world.add(holder, shapes);
        }
    }

    return changed;
}

void NearLianas::resway(World& world, Entity holder, const WindSway& sway, float bend) {
    for (const Entity entity : entities) {
        world.get<ModelRenderer>(entity).sway = sway;
        markRenderChanged(world, entity);
    }

    for (std::shared_ptr<const std::vector<LianaShape>>& block : shapes.slots) {
        if (block) {
            auto bent = std::make_shared<std::vector<LianaShape>>(*block);

            for (LianaShape& shape : *bent) {
                shape.trees[2] = bend;
            }

            block = std::move(bent);
        }
    }

    if (world.has<LianaShapes>(holder)) {
        world.get<LianaShapes>(holder) = shapes;
    }
}

void NearLianas::dropSlot(World& world, std::size_t slot) {
    Slot& dropped = slots[slot];

    for (const Entity entity : dropped.entities) {
        world.destroy(entity);
    }

    dropped = Slot{};
    shapes.slots[slot] = nullptr;
}

void NearLianas::gatherEntities() {
    entities.clear();

    for (const Slot& slot : slots) {
        entities.insert(entities.end(), slot.entities.begin(), slot.entities.end());
    }
}

const std::vector<Entity>& NearLianas::getEntities() const {
    return entities;
}

std::size_t NearLianas::getCellCount() const {
    return ring.getHeld().size();
}

const LianaShapes& NearLianas::getShapes() const {
    return shapes;
}
