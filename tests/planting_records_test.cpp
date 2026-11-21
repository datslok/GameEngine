#include "core/command_registry.h"
#include "engine/engine_commands.h"
#include "engine/settings_file.h"
#include "render/gpu/render_settings.h"
#include "render/gpu/planting_records.h"
#include "scene/planting_cells.h"
#include "scene/planting_maps.h"

#include "planting_helpers.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    // The test maps with a second, odd-sized cap and a bent path, so every map's packing ends part way through a word.
    PlantingMaps oddMaps() {
        PlantingMaps maps = straightMaps();
        maps.pathPoints = {Vec2{10.0f, -1000.0f}, Vec2{12.0f, 0.0f}, Vec2{10.0f, 1000.0f}};
        maps.pathInverseLengths = {1.0f / 1000004.0f, 1.0f / 1000004.0f};
        maps.caps.push_back(PlantingCap{.reach = 80.0f, .origin = Vec2{-30.0f, -40.0f}, .columns = 3, .rows = 3,
                                        .heights = {1, 2, 3, 4, plantingNoCap, 6, 7, 8, 9}});
        maps.axis[7] = 1.5f;
        coverLayer(maps);
        maps.coverMaps.push_back(PlantingCoverMap{.firstColumn = 3, .firstRow = -5, .columns = 3, .rows = 1, .values = {5, 0, 9}});
        return maps;
    }

    // Every map comes back from its buffer as it went in.
    void testTheMapsPackWhole() {
        const PlantingMaps maps = oddMaps();
        const std::vector<std::uint32_t> words = packPlantingMaps(maps);
        GpuPlantingMapsHeader header{};
        std::memcpy(&header, words.data(), sizeof(header));
        assert(header.columns == maps.columns && header.rows == maps.rows && header.inverseTexel == maps.inverseTexel);
        assert(header.pathPieces == 2 && header.capCount == 2);
        assert(header.flags == (PLANTING_HAS_ECOTOPES | PLANTING_HAS_GAPS | PLANTING_HAS_PATH | PLANTING_HAS_CLEARANCE));

        const PlantingMaps back = unpackPlantingMaps(words);
        assert(back.columns == maps.columns && back.rows == maps.rows && back.texelMetres == maps.texelMetres);
        assert(back.halfWidth == maps.halfWidth && back.northmost == maps.northmost && back.texelReach == maps.texelReach);
        assert(back.corridorClearance == maps.corridorClearance && back.axis == maps.axis);
        assert(back.ecotopes == maps.ecotopes && back.gaps == maps.gaps && back.path == maps.path && back.clearance == maps.clearance);
        assert(back.pathPoints.size() == maps.pathPoints.size() && back.pathInverseLengths == maps.pathInverseLengths);

        for (std::size_t point = 0; point < maps.pathPoints.size(); ++point) {
            assert(back.pathPoints[point].x == maps.pathPoints[point].x && back.pathPoints[point].y == maps.pathPoints[point].y);
        }

        assert(back.caps.size() == maps.caps.size());

        for (std::size_t cap = 0; cap < maps.caps.size(); ++cap) {
            assert(back.caps[cap].reach == maps.caps[cap].reach && back.caps[cap].origin.x == maps.caps[cap].origin.x);
            assert(back.caps[cap].origin.y == maps.caps[cap].origin.y && back.caps[cap].columns == maps.caps[cap].columns);
            assert(back.caps[cap].rows == maps.caps[cap].rows && back.caps[cap].heights == maps.caps[cap].heights);
        }

        assert(header.coverMapCount == 2 && back.coverMaps.size() == 2);

        for (std::size_t cover = 0; cover < maps.coverMaps.size(); ++cover) {
            const PlantingCoverMap& each = maps.coverMaps[cover];
            const PlantingCoverMap& returned = back.coverMaps[cover];
            assert(returned.firstColumn == each.firstColumn && returned.firstRow == each.firstRow && returned.columns == each.columns);
            assert(returned.rows == each.rows && returned.values == each.values);
        }

        // Empty maps read as outside: their flags off.
        PlantingMaps bare = maps;
        bare.gaps.clear();
        bare.clearance.clear();
        std::memcpy(&header, packPlantingMaps(bare).data(), sizeof(header));
        assert(header.flags == (PLANTING_HAS_ECOTOPES | PLANTING_HAS_PATH));
    }

    // Each layer as the passes read it: the shares, seed, kinds' tables, the keep-aways' reaches and the spacing's rings.
    void testTheLayersPack() {
        std::vector<PlantingLayerRecord> layers = threeLayers();
        PlantingMaps maps = straightMaps();
        layers.push_back(coverLayer(maps));
        const std::vector<GpuPlantingLayer> packed = packPlantingLayers(layers);
        assert(packed.size() == layers.size() && packed[0].coverMap == -1 && packed[3].coverMap == 0);

        for (std::size_t which = 0; which < layers.size(); ++which) {
            const PlantingLayerRecord& layer = layers[which];
            const GpuPlantingLayer& gpu = packed[which];
            assert(gpu.sharePerWeight[2] == layer.sharePerWeight[2] && gpu.gapStep == layer.gapStep && gpu.footprint == layer.footprint);
            assert(gpu.seed[0] == static_cast<std::uint32_t>(layer.seed) && gpu.seed[1] == static_cast<std::uint32_t>(layer.seed >> 32));
            assert(gpu.under == layer.under && gpu.kinds == layer.kinds && gpu.spacingRings == plantingSpacingRings(layer));
            assert(gpu.sizes == layer.reachPerScale.size() && gpu.reachPerScale[gpu.sizes - 1] == layer.reachPerScale.back());
            assert(gpu.keepAwayCount == layer.keepAway.size());
            assert(gpu.sharedKinds == (layer.kindShares.empty() ? 0u : 1u));

            for (std::size_t keep = 0; keep < layer.keepAway.size(); ++keep) {
                assert(gpu.keepLayer[keep] == layer.keepAway[keep].layer);
                assert(gpu.keepReach[keep] == plantingKeepAwayReach(layer.keepAway[keep], layers));
            }

            for (std::size_t share = 0; share < 4; ++share) {
                assert(gpu.kindShares[share] == (share < layer.kindShares.size() ? layer.kindShares[share] : 2.0f));
            }
        }

        std::vector<PlantingLayerRecord> crowded = layers;
        crowded[2].keepAway.resize(plantingMostKeepAways + 1, PlantingKeepAway{.layer = 0, .radius = 1.0f});
        bool threw = false;

        try {
            packPlantingLayers(crowded);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
    }

    // A job a layer of each cell made: the lattices plantLayers plants over, each cell's own candidates and slot.
    void testTheJobsAreTheCellsBoxes() {
        const std::vector<PlantingLayerRecord> layers = threeLayers();
        const float cellMetres = 64.0f;
        const PlantingSlotLayout layout = plantingSlotLayout(layers, cellMetres);
        const std::vector<PlantingCellMade> made{{PlantingCell{-2, 1}, 5}, {PlantingCell{3, -4}, 0}};
        const std::vector<GpuPlantingJob> jobs = packPlantingJobs(layers, layout, made, cellMetres);
        assert(jobs.size() == made.size() * layers.size());

        for (std::size_t cell = 0; cell < made.size(); ++cell) {
            const PlantingBox box = plantingCellBox(made[cell].cell, cellMetres);
            const std::vector<PlantingLayerBoxes> boxes = plantingLayerBoxes(layers, box);

            for (std::size_t which = 0; which < layers.size(); ++which) {
                const GpuPlantingJob& job = jobs[cell * layers.size() + which];
                const PlantingLattice candidates = plantingLatticeOver(boxes[which].candidates, layers[which].footprint);
                const PlantingLattice standing = plantingLatticeOver(boxes[which].standing, layers[which].footprint);
                const PlantingLattice inBox = plantingLatticeOver(box, layers[which].footprint);
                assert(job.candidates[0] == candidates.firstColumn && job.candidates[3] == candidates.rows);
                assert(job.standing[1] == standing.firstRow && job.standing[2] == standing.columns);
                assert(job.box[0] == inBox.firstColumn && job.box[3] == inBox.rows);
                assert(job.bounds[0] == box.west && job.bounds[1] == box.east && job.bounds[2] == box.north && job.bounds[3] == box.south);
                assert(job.candidateFirst == static_cast<std::uint32_t>(cell) * layout.candidatesACell + layout.scratchFirst[which]);
                assert(job.plantFirst == static_cast<std::uint32_t>(made[cell].slot) * layout.plantsASlot + layout.layerFirst[which]);
                assert(job.countPlace == static_cast<std::uint32_t>(made[cell].slot * 3) + which);
                assert(job.capacity == layout.layerCapacity[which]);
            }
        }
    }

    /*
    * A set's jobs: its plants and counts from its places among every set's, its read-only layers planted (their
    * candidates' lattices) but given no room, never refused for it.
    */
    void testASetsJobsStartAtItsPlaces() {
        std::vector<PlantingLayerRecord> field = threeLayers();
        field[2].cellSet = plantingFineCells;
        const PlantingSetLayers fine = plantingSetLayers(field, plantingFineCells);
        const float cellMetres = 32.0f;
        const PlantingSlotLayout layout = plantingSlotLayout(fine.layers, cellMetres, fine.written);
        const PlantingSetPlaces places{.plantBase = 1000, .countBase = 70};
        const std::vector<PlantingCellMade> made{{PlantingCell{-3, -2}, 4}};
        const std::vector<GpuPlantingJob> jobs = packPlantingJobs(fine.layers, layout, made, cellMetres, places);
        assert(jobs.size() == fine.layers.size());

        for (std::size_t which = 0; which < fine.layers.size(); ++which) {
            const GpuPlantingJob& job = jobs[which];
            assert(job.plantFirst == places.plantBase + 4 * layout.plantsASlot + layout.layerFirst[which]);
            assert(job.countPlace == places.countBase + 4 * static_cast<std::uint32_t>(fine.layers.size()) + which);
            assert(job.capacity == (fine.written[which] ? layout.layerCapacity[which] : 0u));
            assert(job.candidates[2] > 0 && job.candidates[3] > 0);
        }
    }

    // The CPU's whole planting split by cell is each cell planted alone; the same plants compare as agreeing.
    void testTheComparison() {
        const PlantingMaps maps = straightMaps();
        const std::vector<PlantingLayerRecord> layers = threeLayers();
        const float cellMetres = 64.0f;
        const std::vector<PlantingCell> cells{{-2, -3}, {-1, -3}, {0, -2}};
        const PlantingBox around = plantingCellsBox(cells, cellMetres);
        assert(around.west == -128.0f && around.east == 64.0f && around.north == -192.0f && around.south == -64.0f);
        const auto byCell = plantingByCell(plantLayers(layers, maps, rollingGround(), around, testJobs()), cellMetres);
        PlantingComparison comparison;

        for (const PlantingCell& cell : cells) {
            const auto alone = plantLayers(layers, maps, rollingGround(), plantingCellBox(cell, cellMetres), testJobs());
            const auto& split = byCell.at(cell);
            std::vector<std::vector<GpuPlacedPlant>> gpu(layers.size());

            for (std::size_t which = 0; which < layers.size(); ++which) {
                assert(split[which].size() == alone[which].size());

                for (std::size_t i = 0; i < alone[which].size(); ++i) {
                    assert(samePlant(split[which][i], alone[which][i]));
                    gpu[which].push_back(packPlacedPlant(alone[which][i]));
                }
            }

            comparePlantingCell(comparison, alone, gpu);
        }

        assert(comparison.cells == 3 && comparison.layers.size() == 3);

        for (const PlantingLayerComparison& layer : comparison.layers) {
            assert(layer.matching > 0 && layer.missing == 0 && layer.extra == 0 && layer.reordered == 0);
        }

        assert(describePlantingComparison(comparison).find("agree") != std::string::npos);

        // One plant a hair off: the CPU's missing and the GPU's extra; two swapped: the cell reordered.
        const auto alone = plantLayers(layers, maps, rollingGround(), plantingCellBox(cells[0], cellMetres), testJobs());
        std::vector<std::vector<GpuPlacedPlant>> gpu(layers.size());

        for (std::size_t which = 0; which < layers.size(); ++which) {
            for (const PlantedPlant& plant : alone[which]) {
                gpu[which].push_back(packPlacedPlant(plant));
            }
        }

        gpu[2][0].foot[0] = std::nextafter(gpu[2][0].foot[0], 1e9f);
        std::swap(gpu[1][0], gpu[1][1]);
        PlantingComparison differs;
        comparePlantingCell(differs, alone, gpu);
        assert(differs.layers[2].missing == 1 && differs.layers[2].extra == 1);
        assert(differs.layers[1].reordered == 1 && differs.layers[1].missing == 0);
        assert(describePlantingComparison(differs).find("DIFFER") != std::string::npos);

        PlantingComparison troubled;
        troubled.problems[PLANTING_PROBLEM_FULL] = 1;
        assert(describePlantingComparison(troubled).find("DIFFER") != std::string::npos);
    }

    // Each placement-compare call asks for a comparison and shows the last one; it takes no arguments.
    void testTheCompareCommand() {
        CommandRegistry registry;
        int asked = 0;
        std::string last;
        addPlacementCompareCommand(registry, [&]() {
            ++asked;
            return last;
        });

        assert(!registry.execute("placement-compare now").ok && asked == 0);
        const CommandReply first = registry.execute("placement-compare");
        assert(first.ok && asked == 1 && first.text.find("No comparison") != std::string::npos);

        last = describePlantingComparison(PlantingComparison{});
        const CommandReply second = registry.execute("placement-compare");
        assert(second.ok && asked == 2 && second.text.find("agree") != std::string::npos);
    }

    // The cells' settings reach the renderer's under engine.render.planting (F5 Render > Planting), a group a set.
    void testTheSettingsKeys() {
        RenderSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) { visit(settings, visitor); };
        const std::vector<std::string> warnings = applySettings(
            {{"planting.enabled", "true"}, {"planting.coarseCells.cellsAFrame", "8"}, {"planting.middleCells.radius", "500"}, {"planting.fineCells.cell", "16"}},
            walk, StartupFields::Keep);
        assert(warnings.empty());
        assert(settings.planting.enabled && settings.planting.sets[plantingCoarseCells].cellsAFrame == 8);
        assert(settings.planting.sets[plantingMiddleCells].radiusMetres == 500.0f && settings.planting.sets[plantingFineCells].cellMetres == 16.0f);
    }
}

void testPlantingRecords() {
    testTheMapsPackWhole();
    testTheLayersPack();
    testTheJobsAreTheCellsBoxes();
    testASetsJobsStartAtItsPlaces();
    testTheComparison();
    testTheCompareCommand();
    testTheSettingsKeys();
}
