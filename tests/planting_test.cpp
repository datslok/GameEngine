#include "scene/planting.h"
#include "scene/planting_maps.h"
#include "scene/terrain_heights.h"
#include "core/hash.h"

#include "planting_helpers.h"
#include "test_jobs.h"
#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
    bool inBox(const PlantedPlant& plant, const PlantingBox& box) {
        return plant.foot.x >= box.west && plant.foot.x < box.east && plant.foot.z >= box.north && plant.foot.z < box.south;
    }

    // The forest's random numbers are these (forestUnitRandom): SplitMix64 twice, 24 bits; the dither's R2 lattice.
    void testTheRandomNumbersAndTheDither() {
        const std::uint64_t seed = 0x54524545ull;
        assert(plantingRandom(seed, 7) == static_cast<float>(mixBits(seed ^ mixBits(7 + goldenGamma)) >> 40) * (1.0f / 16777216.0f));
        assert(plantingRandom(seed, 7) != plantingRandom(seed, 8) && plantingRandom(seed, 7) != plantingRandom(seed + 1, 7));
        assert(plantingCellKey(-1, 2) == ((0xffffffffull << 32) | 2u));

        int under = 0;
        int inBand = 0;
        std::set<std::pair<int, int>> sparse;

        for (int row = -50; row < 50; ++row) {
            for (int column = -50; column < 50; ++column) {
                const float threshold = plantingThreshold(column, row);
                assert(threshold >= 0.0f && threshold < 1.0f);
                under += threshold < 0.3f ? 1 : 0;
                inBand += threshold >= 0.6f && threshold < 0.7f ? 1 : 0;

                if (threshold < 0.05f) {
                    sparse.insert({column, row});
                }
            }
        }

        assert(under >= 2950 && under <= 3050 && inBand >= 970 && inBand <= 1030);

        // The cells under a small share never touch.
        for (const auto& [column, row] : sparse) {
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    assert((dx == 0 && dz == 0) || !sparse.contains({column + dx, row + dz}));
                }
            }
        }
    }

    // The small functions: the path's blend, the share, the band taken, the kinds, the tests and the ring.
    void testTheMaths() {
        assert(plantingPathShare(10.0f, 20.0f, 60.0f, 0.4f, 1.0f / 40.0f) == 1.0f);
        assert(near(plantingPathShare(40.0f, 20.0f, 60.0f, 0.4f, 1.0f / 40.0f), 0.7f, 1e-6f));
        assert(plantingPathShare(90.0f, 20.0f, 60.0f, 0.4f, 1.0f / 40.0f) == 0.4f);

        assert(near(plantingShare({255.0f, 0.0f, 0.0f}, {0.001f, 0.0f, 0.0f}, 0.0f, 0.0f, 1.0f), 0.255f, 1e-6f));
        assert(near(plantingShare({255.0f, 0.0f, 0.0f}, {0.001f, 0.0f, 0.0f}, 255.0f, -0.5f / 255.0f, 1.0f), 0.1275f, 1e-6f));
        assert(plantingShare({0.0f, 255.0f, 0.0f}, {0.1f, 0.0f, 0.1f}, 0.0f, 0.0f, 1.0f) == 0.0f);
        assert(plantingShare({255.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 1.0f) == 1.0f);

        assert(plantingTaken(0.1f, 0.0f, 0.2f) && !plantingTaken(0.2f, 0.0f, 0.2f));
        assert(!plantingTaken(0.1f, 0.2f, 0.3f) && plantingTaken(0.3f, 0.2f, 0.3f) && plantingTaken(0.6f, 0.2f, 0.9f));
        assert(!plantingTaken(0.99f, 0.5f, 0.3f) && plantingTaken(0.99f, 0.5f, 0.9f));

        assert(plantingUniformKind(0.0f, 4) == 0 && plantingUniformKind(0.25f, 4) == 1 && plantingUniformKind(0.99999f, 4) == 3);
        const std::array<float, plantingMostSharedKinds - 1> cumulative{0.45f, 0.75f, 2.0f, 2.0f};
        assert(plantingSharedKind(0.2f, cumulative, 3) == 0 && plantingSharedKind(0.45f, cumulative, 3) == 1 && plantingSharedKind(0.9f, cumulative, 3) == 2);

        assert(plantingWithin(Vec2{0.0f, 0.0f}, Vec2{3.0f, 4.0f}, 5.01f) && !plantingWithin(Vec2{0.0f, 0.0f}, Vec2{3.0f, 4.0f}, 5.0f));
        assert(plantingFits(5.0f, 5.0f, 10.0f, 2.0f, 12.0f) && !plantingFits(4.9f, 5.0f, 10.0f, 2.0f, 12.0f) && !plantingFits(5.0f, 5.0f, 10.1f, 2.0f, 12.0f));
        assert(plantingTop(1.0f, 20.0f, 0.5f, 3.0f) == 14.0f);
        assert(plantingGivesWay(Vec2{0.0f, 0.0f}, 5.0f, 0.5f, Vec2{3.0f, 0.0f}, 4.0f, 0.4f, false, 0.8f));
        assert(!plantingGivesWay(Vec2{0.0f, 0.0f}, 5.0f, 0.5f, Vec2{3.0f, 0.0f}, 4.0f, 0.6f, true, 0.8f));
        assert(plantingGivesWay(Vec2{0.0f, 0.0f}, 5.0f, 0.5f, Vec2{3.0f, 0.0f}, 4.0f, 0.5f, true, 0.8f));
        assert(!plantingGivesWay(Vec2{0.0f, 0.0f}, 5.0f, 0.5f, Vec2{3.0f, 0.0f}, 4.0f, 0.5f, false, 0.8f));
        assert(!plantingGivesWay(Vec2{0.0f, 0.0f}, 5.0f, 0.5f, Vec2{4.5f, 0.0f}, 4.0f, 0.4f, false, 0.8f));

        for (int k = 0; k < PLANTING_RING_POINTS; ++k) {
            const Vec2 point = plantingRingPoint(Vec2{10.0f, -5.0f}, 2.0f, k);
            const float turn = static_cast<float>(k) * 0.785398163f;
            assert(near(point.x, 10.0f + 2.0f * std::cos(turn), 1e-5f) && near(point.y, -5.0f + 2.0f * std::sin(turn), 1e-5f));
        }

        assert(plantingNearSegment(Vec2{5.0f, 1.0f}, Vec2{0.0f, 0.0f}, Vec2{10.0f, 0.0f}, 0.01f, 1.01f));
        assert(!plantingNearSegment(Vec2{5.0f, 1.0f}, Vec2{0.0f, 0.0f}, Vec2{10.0f, 0.0f}, 0.01f, 1.0f));
        assert(plantingNearSegment(Vec2{12.0f, 0.0f}, Vec2{0.0f, 0.0f}, Vec2{10.0f, 0.0f}, 0.01f, 2.01f));
        assert(!plantingNearSegment(Vec2{12.0f, 0.0f}, Vec2{0.0f, 0.0f}, Vec2{10.0f, 0.0f}, 0.01f, 1.99f));
        assert(plantingBetween(2.0f, 6.0f, 0.25f) == 3.0f && plantingCoordinate(7.0f, -3.0f, 0.5f) == 5.0f);
        assert(plantingStored(123.0f, plantingDecimetre, 1.0f) == 123.0f * plantingDecimetre - 1.0f && plantingPathDistance(-250.0f) == 2.5f);
    }

    // The map reads: blended between texels, bare and far outside, the clearance a lower bound, a cap of the reach asked.
    void testTheMapReads() {
        PlantingMaps maps = straightMaps();
        const std::array<float, 4> middle = plantingEcotopes(maps, 0.0f, -100.0f);
        assert(near(middle[0] + middle[2], 255.0f, 1e-3f) && middle[2] < 2.0f);
        assert(plantingEcotopes(maps, 0.0f, -300.0f)[3] == 255.0f && plantingEcotopes(maps, 200.0f, -100.0f)[3] == 255.0f);
        assert(plantingGap(maps, 0.0f, -100.0f) == 255.0f && plantingGap(maps, 0.0f, -150.0f) == 0.0f);
        assert(near(plantingPathDistanceAt(maps, 13.0f, -100.0f), 3.0f, 0.01f) && near(plantingPathDistanceAt(maps, 7.0f, -60.0f), 3.0f, 0.01f));
        assert(plantingPathDistanceAt(maps, 500.0f, -100.0f) > 300.0f);

        for (float x = -60.0f; x < -20.0f; x += 0.7f) {
            const float truth = std::max(std::sqrt((x + 40.0f) * (x + 40.0f)) - 8.0f, 0.0f);
            assert(plantingClearance(maps, x, -100.0f) <= truth + 1e-4f);
        }

        assert(plantingCap(maps, 10.0f, 0.0f, -100.0f) == plantingUncapped);
        bool threw = false;

        try {
            plantingCap(maps, 41.0f, 0.0f, -100.0f);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);

        // A winding axis is read between rows' middles in straight lines.
        maps.axis[10] = 4.0f;
        maps.axis[11] = 8.0f;
        const float between = maps.northmost + 11.0f * maps.texelMetres;
        assert(plantingAxisAt(maps, between) == 6.0f && plantingAxisAt(maps, -1000.0f) == 0.0f);
    }

    /*
    * The layers planted: about their share of cells, clear of the path, their own kind's spacing and the layers before,
    * on the ground, every kind used, the same every run; a layer reading itself or a later one, or sharing a lattice of
    * another footprint, throws.
    */
    void testTheLayersArePlanted() {
        const PlantingMaps maps = straightMaps();
        const std::vector<PlantingLayerRecord> layers = threeLayers();
        const PlantingBox box{-100.0f, 100.0f, -180.0f, -20.0f};
        const std::vector<std::vector<PlantedPlant>> planted = plantLayers(layers, maps, rollingGround(), box, testJobs());
        assert(planted.size() == 3 && planted[0].size() > 30 && planted[1].size() > 300 && planted[2].size() > 3000);

        for (std::size_t which = 0; which < planted.size(); ++which) {
            const PlantingLayerRecord& layer = layers[which];
            std::set<int> kinds;

            for (std::size_t i = 0; i < planted[which].size(); ++i) {
                const PlantedPlant& plant = planted[which][i];
                const Vec2 place{plant.foot.x, plant.foot.z};
                assert(inBox(plant, box));
                assert(std::fabs(plant.foot.x - 10.0f) >= layer.pathClearance - 1e-4f && plantingClearOfThePath(maps, place, layer.pathClearance));
                assert(plant.scale >= layer.leastScale && plant.scale <= layer.leastScale + layer.scaleRange);
                assert(plant.threshold == plantingThreshold(plant.column, plant.row));
                assert(std::fabs(place.x - (static_cast<float>(plant.column) + 0.5f) * layer.footprint) <= 0.5f * layer.jitterMetres + 1e-4f);
                assert(plantingClearance(maps, place.x, place.y) >= plant.reach);
                kinds.insert(plant.kind);

                const float ground = terrainSurfaceHeight(rollingGround(), place.x, place.y);

                if (layer.footRingPerScale > 0.0f) {
                    assert(plant.foot.y <= ground && plant.foot.y > ground - 2.0f);
                } else {
                    assert(plant.foot.y == ground - layer.footSink);
                }

                for (std::size_t j = 0; j < i && layer.spacingShare > 0.0f; ++j) {
                    const PlantedPlant& other = planted[which][j];
                    assert(!plantingWithin(place, Vec2{other.foot.x, other.foot.z}, layer.spacingShare * std::max(plant.reach, other.reach)));
                }

                for (const PlantingKeepAway& keep : layer.keepAway) {
                    for (const PlantedPlant& other : planted[static_cast<std::size_t>(keep.layer)]) {
                        assert(!plantingWithin(place, Vec2{other.foot.x, other.foot.z}, keep.crown ? other.reach : keep.radius));
                    }
                }
            }

            assert(static_cast<int>(kinds.size()) == layer.kinds);
        }

        // The ground plants are thickest by the path and in the gap.
        int nearPath = 0;
        int farPath = 0;

        for (const PlantedPlant& plant : planted[2]) {
            nearPath += std::fabs(plant.foot.x - 10.0f) < 15.0f && std::fabs(plant.foot.z + 150.0f) < 20.0f ? 1 : 0;
            farPath += std::fabs(plant.foot.x - 80.0f) < 15.0f && std::fabs(plant.foot.z + 150.0f) < 20.0f ? 1 : 0;
        }

        assert(nearPath > 2 * farPath && farPath > 0);

        const std::vector<std::vector<PlantedPlant>> again = plantLayers(layers, maps, rollingGround(), box, serialJobs());

        for (std::size_t which = 0; which < planted.size(); ++which) {
            assert(again[which].size() == planted[which].size() && samePlant(again[which].back(), planted[which].back()));
        }

        const auto throws = [&](std::vector<PlantingLayerRecord> bad) {
            try {
                plantLayers(bad, maps, rollingGround(), box, testJobs());
            } catch (const std::invalid_argument&) {
                return true;
            }

            return false;
        };

        std::vector<PlantingLayerRecord> readsItself = layers;
        readsItself[1].keepAway.push_back(PlantingKeepAway{.layer = 1, .radius = 1.0f});
        std::vector<PlantingLayerRecord> otherLattice = layers;
        otherLattice[1].under = 0;
        std::vector<PlantingLayerRecord> tooManyKinds = layers;
        tooManyKinds[0].kinds = plantingMostKinds + 1;
        assert(throws(readsItself) && throws(otherLattice) && throws(tooManyKinds));
    }

    /*
    * A box planted alone has exactly the plants planting a larger one puts in it, however the larger one is cut and in
    * whatever order the pieces are planted: what lets the cells round the camera be planted one by one, on either side.
    */
    void testABoxAloneIsTheSameAsTheWhole() {
        PlantingMaps maps = straightMaps();
        std::vector<PlantingLayerRecord> layers = threeLayers();
        layers.push_back(coverLayer(maps));
        const PlantingBox whole{-100.0f, 100.0f, -180.0f, -20.0f};
        const std::vector<std::vector<PlantedPlant>> all = plantLayers(layers, maps, rollingGround(), whole, testJobs());

        for (const float cell : {32.0f, 50.0f}) {
            std::vector<std::vector<PlantedPlant>> pieced(layers.size());
            std::vector<PlantingBox> pieces;

            for (float north = whole.north; north < whole.south; north += cell) {
                for (float west = whole.west; west < whole.east; west += cell) {
                    pieces.push_back(PlantingBox{west, std::min(west + cell, whole.east), north, std::min(north + cell, whole.south)});
                }
            }

            // Planted from the far end first.
            std::reverse(pieces.begin(), pieces.end());

            for (const PlantingBox& piece : pieces) {
                const std::vector<std::vector<PlantedPlant>> planted = plantLayers(layers, maps, rollingGround(), piece, testJobs());

                for (std::size_t which = 0; which < layers.size(); ++which) {
                    pieced[which].insert(pieced[which].end(), planted[which].begin(), planted[which].end());
                }
            }

            for (std::size_t which = 0; which < layers.size(); ++which) {
                std::vector<PlantedPlant> sorted = pieced[which];
                std::sort(sorted.begin(), sorted.end(), [](const PlantedPlant& a, const PlantedPlant& b) {
                    return a.row != b.row ? a.row < b.row : a.column < b.column;
                });
                assert(sorted.size() == all[which].size());

                for (std::size_t i = 0; i < sorted.size(); ++i) {
                    assert(samePlant(sorted[i], all[which][i]));
                }
            }
        }
    }

    // A layer sharing another's lattice plants the band of thresholds above the other's share: never the same cell.
    void testASharedLatticeTakesTheBandAbove() {
        const PlantingMaps maps = straightMaps();
        std::vector<PlantingLayerRecord> layers = threeLayers();
        PlantingLayerRecord second = layers[0];
        second.seed = 0x4321u;
        second.under = 0;
        second.spacingShare = 0.0f;
        second.keepAway.clear();
        setPlantingDensity(second, PlantingDensity{.perHectare = {200.0f, 0.0f, 100.0f}, .footprintMetres = 12.0f, .jitterShare = 0.8f});
        const std::vector<std::vector<PlantedPlant>> planted =
            plantLayers({layers[0], second}, maps, rollingGround(), PlantingBox{-100.0f, 100.0f, -180.0f, -20.0f}, testJobs());
        std::set<std::pair<int, int>> taken;

        for (const PlantedPlant& plant : planted[0]) {
            taken.insert({plant.column, plant.row});
        }

        assert(!planted[1].empty());

        for (const PlantedPlant& plant : planted[1]) {
            assert(!taken.contains({plant.column, plant.row}));
            assert(plant.threshold >= plantingLayerShare(layers[0], maps, (static_cast<float>(plant.column) + 0.5f) * 12.0f,
                                                         (static_cast<float>(plant.row) + 0.5f) * 12.0f));
        }
    }
}

namespace {
    // A cover byte gives back its kind and quarter turns, every kind with every turn; the turn exactly so many quarters.
    void testTheCoverBytes() {
        for (int kind = 0; kind < plantingMostKinds; ++kind) {
            for (int quarters = 0; quarters < 4; ++quarters) {
                const int value = plantingCoverValue(kind, quarters);
                assert(value >= 1 && value <= 255 && plantingCoverKind(value) == kind);
                assert(plantingCoverTurn(value) == static_cast<float>(quarters) * static_cast<float>(PLANTING_QUARTER_TURN));
            }
        }

        const PlantingCoverMap map{.firstColumn = -2, .firstRow = 3, .columns = 2, .rows = 2, .values = {1, 2, 3, 4}};
        assert(plantingCoverAt(map, -2, 3) == 1 && plantingCoverAt(map, -1, 4) == 4);
        assert(plantingCoverAt(map, -3, 3) == 0 && plantingCoverAt(map, 0, 3) == 0 && plantingCoverAt(map, -2, 5) == 0);
    }

    /*
    * A cover layer after the others plants exactly the cells its map names, whatever the dither, the path and the obstacle
    * would say: one plant a named cell whose middle the box holds, at that middle on the drawn ground, its kind and turn
    * the byte's, its scale and pick drawn as any layer's.
    */
    void testACoverLayerPlantsItsMap() {
        PlantingMaps maps = straightMaps();
        std::vector<PlantingLayerRecord> layers = threeLayers();
        layers.push_back(coverLayer(maps));
        const PlantingBox box{-100.0f, 100.0f, -180.0f, -20.0f};
        const std::vector<PlantedPlant> planted = plantLayers(layers, maps, rollingGround(), box, testJobs()).back();
        const PlantingCoverMap& map = maps.coverMaps.back();
        std::size_t named = 0;

        for (int row = map.firstRow; row < map.firstRow + map.rows; ++row) {
            for (int column = map.firstColumn; column < map.firstColumn + map.columns; ++column) {
                const float x = (static_cast<float>(column) + 0.5f) * 8.0f;
                const float z = (static_cast<float>(row) + 0.5f) * 8.0f;
                named += plantingCoverAt(map, column, row) != 0 && x >= box.west && x < box.east && z >= box.north && z < box.south ? 1 : 0;
            }
        }

        assert(planted.size() == named && named > 300);

        for (const PlantedPlant& plant : planted) {
            const int value = plantingCoverAt(map, plant.column, plant.row);
            assert(value != 0 && plant.kind == plantingCoverKind(value) && plant.turnRadians == plantingCoverTurn(value));
            assert(plant.foot.x == (static_cast<float>(plant.column) + 0.5f) * 8.0f && plant.foot.z == (static_cast<float>(plant.row) + 0.5f) * 8.0f);
            assert(plant.foot.y == terrainSurfaceHeight(rollingGround(), plant.foot.x, plant.foot.z));
            assert(plant.scale == 1.0f && plant.reach == 5.7f && plant.pick >= 0.0f && plant.pick < 1.0f);
        }
    }

    // A cover layer may not share a lattice or keep away; its map must exist, be its size and name only its kinds.
    void testCoverLayersAreChecked() {
        const auto refused = [](const std::vector<PlantingLayerRecord>& layers, const PlantingMaps& maps) {
            try {
                plantLayers(layers, maps, rollingGround(), PlantingBox{-10.0f, 10.0f, -60.0f, -40.0f}, testJobs());
            } catch (const std::invalid_argument&) {
                return true;
            }

            return false;
        };

        PlantingMaps maps = straightMaps();
        std::vector<PlantingLayerRecord> layers = threeLayers();
        layers.push_back(coverLayer(maps));
        assert(!refused(layers, maps));
        layers.back().keepAway = {PlantingKeepAway{.layer = 0, .radius = 2.0f}};
        assert(refused(layers, maps));
        layers.back().keepAway.clear();
        layers.back().coverMap = 1;
        assert(refused(layers, maps));
        layers.back().coverMap = 0;
        layers.back().kinds = 7;
        assert(refused(layers, maps));
        layers.back().kinds = 8;
        maps.coverMaps.back().values.pop_back();
        assert(refused(layers, maps));
    }
}

void testPlanting() {
    testTheCoverBytes();
    testACoverLayerPlantsItsMap();
    testCoverLayersAreChecked();
    testTheRandomNumbersAndTheDither();
    testTheMaths();
    testTheMapReads();
    testTheLayersArePlanted();
    testABoxAloneIsTheSameAsTheWhole();
    testASharedLatticeTakesTheBandAbove();
}
