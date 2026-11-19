#include "scene/planting_cells.h"
#include "scene/planting_maps.h"

#include "forest_helpers.h"
#include "planting_helpers.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
    // The squared distance from a place to a cell's nearest point.
    float nearestSquared(const PlantingCell& cell, float cellMetres, const Vec2& place) {
        const PlantingBox box = plantingCellBox(cell, cellMetres);
        const float dx = std::max({box.west - place.x, 0.0f, place.x - box.east});
        const float dz = std::max({box.north - place.y, 0.0f, place.y - box.south});
        return dx * dx + dz * dz;
    }

    // Every cell within reach of a place, as the ring should hold them once it has caught up.
    std::set<PlantingCell> cellsWithin(const Vec2& place, float cellMetres, float reach) {
        std::set<PlantingCell> cells;
        const int first = static_cast<int>(std::floor((place.x - reach) / cellMetres)) - 1;
        const int last = static_cast<int>(std::floor((place.x + reach) / cellMetres)) + 1;
        const int firstRow = static_cast<int>(std::floor((place.y - reach) / cellMetres)) - 1;
        const int lastRow = static_cast<int>(std::floor((place.y + reach) / cellMetres)) + 1;

        for (int row = firstRow; row <= lastRow; ++row) {
            for (int column = first; column <= last; ++column) {
                if (nearestSquared(PlantingCell{column, row}, cellMetres, place) <= reach * reach) {
                    cells.insert(PlantingCell{column, row});
                }
            }
        }

        return cells;
    }

    // Updates until a frame makes nothing, returning every cell made in order.
    std::vector<PlantingCellMade> catchUp(PlantingCellRing& ring, const Vec2& camera, const PlantingCellSettings& settings) {
        std::vector<PlantingCellMade> made;

        for (int frame = 0; frame < 10000; ++frame) {
            const std::vector<PlantingCellMade> now = ring.update(camera, settings);

            if (now.empty()) {
                return made;
            }

            assert(static_cast<int>(now.size()) <= settings.cellsAFrame);
            made.insert(made.end(), now.begin(), now.end());
        }

        assert(false);
        return made;
    }

    // Each slot holds one cell, every slot within the count.
    void checkSlots(const PlantingCellRing& ring) {
        std::set<int> slots;

        for (const auto& [cell, slot] : ring.getHeld()) {
            assert(slot >= 0 && slot < ring.getSlotCount());
            assert(slots.insert(slot).second);
        }
    }

    /*
    * The ring makes the nearest cells first, a few a frame, until every cell within the radius is held, each in its own
    * slot; a frame later nothing more is made.
    */
    void testTheRingFillsNearestFirst() {
        const PlantingCellSettings settings{.enabled = true, .cellMetres = 50.0f, .radiusMetres = 200.0f, .cellsAFrame = 3};
        PlantingCellRing ring;
        const Vec2 camera{12.0f, -31.0f};
        const std::vector<PlantingCellMade> first = ring.update(camera, settings);
        assert(first.size() == 3);
        assert(first[0].cell == (PlantingCell{0, -1}));
        assert(first[0].slot == 0 && first[1].slot == 1 && first[2].slot == 2);

        std::vector<PlantingCellMade> made = first;
        const std::vector<PlantingCellMade> rest = catchUp(ring, camera, settings);
        made.insert(made.end(), rest.begin(), rest.end());

        for (std::size_t i = 1; i < made.size(); ++i) {
            assert(nearestSquared(made[i - 1].cell, settings.cellMetres, camera) <= nearestSquared(made[i].cell, settings.cellMetres, camera));
        }

        const std::set<PlantingCell> wanted = cellsWithin(camera, settings.cellMetres, settings.radiusMetres);
        assert(ring.getHeld().size() == wanted.size());

        for (const PlantingCell& cell : wanted) {
            assert(ring.getHeld().contains(cell));
        }

        checkSlots(ring);
        assert(ring.update(camera, settings).empty());
    }

    /*
    * Walking away, cells a cell past the radius are dropped and their slots taken by the new ones, lowest first; the held
    * cells never outnumber the slots; the same walk makes the same cells in the same slots.
    */
    void testTheRingFollowsTheCamera() {
        const PlantingCellSettings settings{.enabled = true, .cellMetres = 40.0f, .radiusMetres = 150.0f, .cellsAFrame = 4};
        const auto walk = [&](std::vector<PlantingCellMade>& made) {
            PlantingCellRing ring;

            for (int step = 0; step < 400; ++step) {
                const float t = static_cast<float>(step);
                const Vec2 camera{t * 7.3f - 900.0f, 300.0f * std::sin(t * 0.02f)};
                const std::vector<PlantingCellMade> now = ring.update(camera, settings);
                made.insert(made.end(), now.begin(), now.end());
                checkSlots(ring);
                assert(static_cast<int>(ring.getHeld().size()) <= ring.getSlotCount());

                for (const auto& [cell, slot] : ring.getHeld()) {
                    const float drop = settings.radiusMetres + settings.cellMetres;
                    assert(nearestSquared(cell, settings.cellMetres, camera) <= drop * drop);
                }
            }

            return ring.getHeld();
        };

        std::vector<PlantingCellMade> once;
        std::vector<PlantingCellMade> twice;
        const std::map<PlantingCell, int> heldOnce = walk(once);
        const std::map<PlantingCell, int> heldTwice = walk(twice);
        assert(heldOnce == heldTwice);
        assert(once.size() == twice.size() && once.size() > heldOnce.size());

        for (std::size_t i = 0; i < once.size(); ++i) {
            assert(once[i].cell == twice[i].cell && once[i].slot == twice[i].slot);
        }
    }

    // A teleport drops every cell behind; a new radius or cell size forgets them all and starts again from slot 0.
    void testTheRingStartsAgain() {
        PlantingCellSettings settings{.enabled = true, .cellMetres = 64.0f, .radiusMetres = 300.0f, .cellsAFrame = 4};
        PlantingCellRing ring;
        catchUp(ring, Vec2{0.0f, 0.0f}, settings);
        const std::vector<PlantingCellMade> far = ring.update(Vec2{5000.0f, 5000.0f}, settings);
        assert(far.size() == 4);

        for (const auto& [cell, slot] : ring.getHeld()) {
            assert(nearestSquared(cell, settings.cellMetres, Vec2{5000.0f, 5000.0f}) <= 300.0f * 300.0f);
        }

        assert(far[0].slot == 0 && far[3].slot == 3);

        settings.radiusMetres = 200.0f;
        const std::vector<PlantingCellMade> renewed = ring.update(Vec2{5000.0f, 5000.0f}, settings);
        assert(ring.getHeld().size() == renewed.size() && renewed.front().slot == 0);

        ring.reset();
        assert(ring.getHeld().empty());
    }

    // A slot holds every layer's plants in turn, each room for its lattice over any cell; the scratch every layer's candidates.
    void testTheSlotsHoldEveryCell() {
        const std::vector<PlantingLayerRecord> layers = threeLayers();
        const float cellMetres = 64.0f;
        const PlantingSlotLayout layout = plantingSlotLayout(layers, cellMetres);
        assert(layout.layerFirst.size() == layers.size() && layout.scratchFirst.size() == layers.size());
        std::uint32_t plants = 0;
        std::uint32_t candidates = 0;

        for (std::size_t which = 0; which < layers.size(); ++which) {
            assert(layout.layerFirst[which] == plants && layout.scratchFirst[which] == candidates);
            plants += layout.layerCapacity[which];
            candidates += layout.scratchCapacity[which];
        }

        assert(layout.plantsASlot == plants && layout.candidatesACell == candidates);

        for (int row = -40; row < 40; row += 3) {
            for (int column = -40; column < 40; column += 7) {
                const PlantingBox box = plantingCellBox(PlantingCell{column, row}, cellMetres);
                const std::vector<PlantingLayerBoxes> boxes = plantingLayerBoxes(layers, box);

                for (std::size_t which = 0; which < layers.size(); ++which) {
                    const float footprint = layers[which].footprint;
                    assert(plantingLatticeOver(box, footprint).count() <= layout.layerCapacity[which]);
                    assert(plantingLatticeOver(boxes[which].candidates, footprint).count() <= layout.scratchCapacity[which]);
                    assert(boxes[which].standing.west <= box.west && boxes[which].standing.south >= box.south);
                }
            }
        }

        // The ground plants' lattice dominates: (64 / 1.5 rounded up + 4) squared.
        assert(layout.layerCapacity[2] == 47u * 47u);
    }

    // The slots cover every cell within the drop distance wherever the camera stands.
    void testTheSlotCountCoversTheDropDistance() {
        const PlantingCellSettings settings{.enabled = true, .cellMetres = 64.0f, .radiusMetres = 300.0f, .cellsAFrame = 4};
        const int slots = plantingSlotCount(settings);
        assert(slots == 13 * 13);

        for (const Vec2 place : {Vec2{0.0f, 0.0f}, Vec2{31.9f, 32.1f}, Vec2{-63.99f, 0.01f}}) {
            assert(static_cast<int>(cellsWithin(place, settings.cellMetres, settings.radiusMetres + settings.cellMetres).size()) <= slots);
        }
    }

    // plantLayers plants from the boxes plantingLayerBoxes gives: the patch planted cell by cell is the whole.
    void testTheCellsPlantTheWhole() {
        const PlantingMaps maps = straightMaps();
        const std::vector<PlantingLayerRecord> layers = threeLayers();
        const float cellMetres = 64.0f;
        const PlantingBox whole = {-128.0f, 64.0f, -192.0f, -64.0f};
        const std::vector<std::vector<PlantedPlant>> all = plantLayers(layers, maps, rollingGround(), whole, testJobs());
        std::vector<std::size_t> counts(layers.size(), 0);

        for (int row = -3; row < -1; ++row) {
            for (int column = -2; column < 1; ++column) {
                const std::vector<std::vector<PlantedPlant>> cell =
                    plantLayers(layers, maps, rollingGround(), plantingCellBox(PlantingCell{column, row}, cellMetres), testJobs());

                for (std::size_t which = 0; which < layers.size(); ++which) {
                    counts[which] += cell[which].size();
                }
            }
        }

        for (std::size_t which = 0; which < layers.size(); ++which) {
            assert(counts[which] == all[which].size() && !all[which].empty());
        }
    }

    void testTheSettingsAreVisited() {
        PlantingCellSettings settings;
        assert(!settings.enabled);
        FieldCounter counter;
        visit(settings, counter);
        assert(counter.fields == 4);
    }
}

void testPlantingCells() {
    testTheRingFillsNearestFirst();
    testTheRingFollowsTheCamera();
    testTheRingStartsAgain();
    testTheSlotsHoldEveryCell();
    testTheSlotCountCoversTheDropDistance();
    testTheCellsPlantTheWhole();
    testTheSettingsAreVisited();
}
