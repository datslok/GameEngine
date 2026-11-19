#include "games/forest/valley_maps.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/forest_trees.h"
#include "core/job_system.h"

#include "forest_helpers.h"
#include "test_jobs.h"
#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
    float weightSum(const EcotopeWeights& weights) {
        return std::accumulate(weights.begin(), weights.end(), 0.0f);
    }

    float ecotope(const EcotopeWeights& weights, Ecotope which) {
        return weights[static_cast<std::size_t>(which)];
    }

    // A small extent for the tests that build their own maps.
    ForestPlantingSettings smallExtent() {
        ForestPlantingSettings settings;
        settings.northmost = -300.0f;
        settings.southmost = -60.0f;
        settings.halfWidth = 120.0f;
        return settings;
    }

    // The planting settings and every layer's fields, each named once, all read at start-up.
    void testTheSettings() {
        ForestPlantingSettings settings;
        FieldCounter counter;
        visit(settings, counter);
        assert(counter.fields == 12 && counter.live == 0);

        PlantingLayer layer;
        FieldCounter layerCounter;
        visit(layer, layerCounter);
        assert(layerCounter.fields == 10 && layerCounter.live == 0);

        // The floor, walls and rim (the user's extent, Nov 19 2026), not the mountains beyond.
        assert(settings.halfWidth >= valleyFloorHalfWidth + valleyWallWidth && settings.halfWidth <= valleyFloorHalfWidth + valleyWallWidth + 150.0f);
        assert(settings.texelMetres == 2.0f && settings.southmost >= forestStartPlace.y);
    }

    /*
    * The ecotopes from the land: floor jungle on the floor, wall on the walls, rim forest past them, bare in the clearing,
    * the meadow, on cliffs and outside the extent; their weights always sum to 1.
    */
    void testTheLandsEcotopes() {
        const ForestPlantingSettings settings;
        const auto at = [&](float across, float z, float slope = 0.0f) {
            const EcotopeWeights weights = landEcotopes(settings, valleyAxis(z) + across, z, slope);
            assert(near(weightSum(weights), 1.0f, 1e-5f));
            return weights;
        };

        assert(ecotope(at(0.0f, -1500.0f), Ecotope::Floor) == 1.0f);
        assert(ecotope(at(valleyFloorHalfWidth + 0.5f * valleyWallWidth, -1500.0f), Ecotope::Wall) == 1.0f);
        assert(ecotope(at(valleyFloorHalfWidth + valleyWallWidth + 60.0f, -1500.0f), Ecotope::Rim) == 1.0f);
        assert(ecotope(at(-(valleyFloorHalfWidth + valleyWallWidth + 60.0f), -1500.0f), Ecotope::Rim) == 1.0f);

        // Half and half where the floor meets the wall.
        assert(near(ecotope(at(valleyFloorHalfWidth, -1500.0f), Ecotope::Floor), 0.5f, 1e-4f));

        assert(ecotope(at(0.0f, forestClearingCentre.y), Ecotope::Bare) == 1.0f);
        assert(ecotope(landEcotopes(settings, forestStartPlace.x, forestStartPlace.y, 0.0f), Ecotope::Bare) == 1.0f);
        assert(ecotope(at(0.0f, -1500.0f, settings.bareSlopeTo), Ecotope::Bare) == 1.0f);
        assert(ecotope(at(0.0f, -1500.0f, 0.5f * (settings.bareSlopeFrom + settings.bareSlopeTo)), Ecotope::Bare) > 0.0f);
        assert(ecotope(at(settings.halfWidth + 1.0f, -1500.0f), Ecotope::Bare) == 1.0f);
        assert(ecotope(at(0.0f, settings.northmost - 1.0f), Ecotope::Bare) == 1.0f);

        // The patch is all floor (its forest as before) but a little at its northern edge, by the clearing.
        assert(ecotope(at(0.0f, -100.0f), Ecotope::Floor) > 0.99f);
    }

    /*
    * The maps: their size by the extent, each texel's ecotopes summing to 255, the same built on every core as on one,
    * read back as the land gives them.
    */
    void testTheMapsAreBuilt() {
        const ForestPlantingSettings settings = smallExtent();
        const ValleyMaps maps = buildValleyMaps(settings, {}, {}, {}, testJobs());
        assert(maps.columns == 120 && maps.rows == 120);
        assert(maps.ecotopes.size() == 120u * 120u && maps.clearance.size() == maps.ecotopes.size() && maps.caps.empty());
        assert(maps.texelReach >= 0.5f * std::sqrt(2.0f) * settings.texelMetres && maps.texelReach < settings.texelMetres);

        for (const auto& texel : maps.ecotopes) {
            assert(texel[0] + texel[1] + texel[2] + texel[3] == 255);
        }

        const ValleyMaps serial = buildValleyMaps(settings, {}, {}, {}, serialJobs());
        assert(serial.ecotopes == maps.ecotopes && serial.clearance == maps.clearance);

        // Read back: the floor in the patch, bare in the clearing, all bare outside.
        assert(ecotope(valleyEcotopes(maps, valleyAxis(-100.0f), -100.0f), Ecotope::Floor) > 0.98f);
        assert(ecotope(valleyEcotopes(maps, forestClearingCentre.x, forestClearingCentre.y + 40.0f), Ecotope::Bare) > 0.98f);
        assert(ecotope(valleyEcotopes(maps, 0.0f, -1000.0f), Ecotope::Bare) == 1.0f);
        assert(near(weightSum(valleyEcotopes(maps, 13.3f, -151.7f)), 1.0f, 1e-4f));

        // Blended between texels: smooth across the clearing's edge.
        const float z = forestClearingCentre.y + forestClearingRadius + 0.5f * settings.clearingEdgeMetres;
        const float bare = ecotope(valleyEcotopes(maps, forestClearingCentre.x, z), Ecotope::Bare);
        assert(bare > 0.2f && bare < 0.8f);
    }

    /*
    * The clearance: never more than the true distance to the nearest obstacle's edge (so nothing is planted into one),
    * and within two texels' reach of it; far from every obstacle, at least the farthest it tells apart (less the reach).
    */
    void testTheClearance() {
        const ForestPlantingSettings settings = smallExtent();
        const std::vector<ValleyObstacle> obstacles{{Vec2{30.0f, -150.0f}, 12.0f}, {Vec2{-60.0f, -220.0f}, 25.0f}};
        const ValleyMaps maps = buildValleyMaps(settings, obstacles, {}, {}, testJobs());

        for (int i = 0; i < 2000; ++i) {
            const float z = -290.0f + 220.0f * forestUnitRandom(5, 2 * i);
            const float x = valleyAxis(z) + 110.0f * (2.0f * forestUnitRandom(5, 2 * i + 1) - 1.0f);
            float truth = 1e9f;

            for (const ValleyObstacle& obstacle : obstacles) {
                truth = std::min(truth, std::max((Vec2{x, z} - obstacle.centre).length() - obstacle.radius, 0.0f));
            }

            const float read = valleyClearance(maps, x, z);
            assert(read <= truth + 1e-4f);

            if (truth < farthestClearance - 2.0f * maps.texelReach) {
                assert(read >= truth - 2.0f * maps.texelReach - 0.1f);
            } else {
                assert(read >= std::min(truth, farthestClearance) - 2.0f * maps.texelReach - 0.1f);
            }
        }

        const ValleyMaps empty = buildValleyMaps(settings, {}, {}, {}, testJobs());
        assert(valleyClearance(empty, valleyAxis(-150.0f), -150.0f) >= farthestClearance - empty.texelReach - 0.1f);
    }

    // The lowest the line passes within reach of a place, found by walking it finely.
    float lowestWithin(const SightLine& line, float reach, float x, float z) {
        float lowest = 1e9f;

        for (int step = 0; step <= 20000; ++step) {
            const Vec3 point = line.from + (line.to - line.from) * (static_cast<float>(step) / 20000.0f);

            if ((Vec2{point.x, point.z} - Vec2{x, z}).length() <= reach) {
                lowest = std::min(lowest, point.y);
            }
        }

        return lowest;
    }

    /*
    * The canopy caps: never above a line within reach of a place (so a crown under the cap meets none), and not far
    * below where one passes overhead; very high where none passes; a reach no cap was built for throws.
    */
    void testTheCanopyCaps() {
        const ForestPlantingSettings settings = smallExtent();
        const SightLine line{Vec3{0.0f, 1.7f, -80.0f}, Vec3{0.0f, 250.0f, -400.0f}};
        const ValleyMaps maps = buildValleyMaps(settings, {}, {line}, {16.0f, 4.0f}, testJobs());
        assert(maps.caps.size() == 2);

        for (int i = 0; i < 400; ++i) {
            const float z = -280.0f + 200.0f * forestUnitRandom(7, 2 * i);
            const float x = 60.0f * (2.0f * forestUnitRandom(7, 2 * i + 1) - 1.0f);

            for (const float reach : {16.0f, 4.0f}) {
                const float lowest = lowestWithin(line, reach, x, z);
                const float cap = valleyCap(maps, reach, x, z);
                assert(cap <= lowest + 1e-3f);

                if (lowest < 1e8f) {
                    assert(cap >= lowestWithin(line, reach + 3.0f * settings.texelMetres, x, z) - 2.0f);
                }
            }
        }

        // A smaller reach reads the smallest cap at least as far.
        assert(valleyCap(maps, 3.0f, 0.0f, -200.0f) == valleyCap(maps, 4.0f, 0.0f, -200.0f));
        assert(valleyCap(maps, 4.0f, 50.0f, -200.0f) > 1e6f);
        assert(valleyCap(maps, 4.0f, 0.0f, -2000.0f) > 1e6f);

        bool threw = false;

        try {
            valleyCap(maps, 17.0f, 0.0f, -200.0f);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
    }

    /*
    * The test the placements rely on: a sphere the maps let stand meets no sight line and clears every obstacle, whether
    * low or high, near a line or not (the user's sight lines, now corridors).
    */
    void testWhatFitsBlocksNoSight() {
        const ValleyMaps& maps = patchValleyMaps();
        const std::vector<SightLine> lines = forestSightLines();
        std::vector<BastionShape> towers = farTowers();
        towers.push_back(monolithBastion(forestHeight(forestClearingCentre.x, forestClearingCentre.y), ForestBastionSettings{}));
        const std::vector<ValleyObstacle> obstacles = forestObstacles(placePillars(ValleyLayout{}), towers);
        int fitted = 0;
        int refused = 0;

        for (int i = 0; i < 4000; ++i) {
            const float z = -400.0f + 480.0f * forestUnitRandom(9, 4 * i);
            const float x = valleyAxis(z) + 250.0f * (2.0f * forestUnitRandom(9, 4 * i + 1) - 1.0f);
            const float reach = 1.0f + 15.0f * forestUnitRandom(9, 4 * i + 2);
            const float centre = forestHeight(x, z) + 60.0f * forestUnitRandom(9, 4 * i + 3);

            if (!fitsTheMaps(maps, Vec2{x, z}, std::min(reach, maps.caps.back().reach), centre + reach)) {
                ++refused;
                continue;
            }

            ++fitted;
            assert(!blocksSight(Vec3{x, centre, z}, std::min(reach, maps.caps.back().reach), lines));

            for (const ValleyObstacle& obstacle : obstacles) {
                assert((Vec2{x, z} - obstacle.centre).length() >= obstacle.radius + std::min(reach, maps.caps.back().reach) - 1e-3f);
            }
        }

        assert(fitted > 400 && refused > 400);
    }

    /*
    * The obstacles: one a pillar (its footprint grown by its bulge), the Ring's two feet, one a tower round its foot,
    * reaching its widest section's corners.
    */
    void testTheObstacles() {
        const std::vector<PillarPlacement> pillars{{Vec3{100.0f, 0.0f, -900.0f}, 40.0f, 300.0f, 0.0f, 1u}};
        const BastionShape tower = farTowers().front();
        const std::vector<ValleyObstacle> obstacles = forestObstacles(pillars, {tower});
        assert(obstacles.size() == 4);
        assert(obstacles[0].centre.x == 100.0f && obstacles[0].centre.y == -900.0f && near(obstacles[0].radius, pillarBulgeShare * 40.0f, 1e-4f));
        assert(near(obstacles[1].centre.x, forestRingPlace.x - forestRingRadius, 1e-4f) && near(obstacles[2].centre.x, forestRingPlace.x + forestRingRadius, 1e-4f));
        assert(near(obstacles[1].radius, ringFootShare * forestRingTubeRadius, 1e-4f));

        float widest = tower.glacisWidth;

        for (const BastionTier& tier : tower.tiers) {
            widest = std::max(widest, tier.width);
        }

        assert(obstacles[3].centre.x == tower.foot.x && obstacles[3].centre.y == tower.foot.z);
        assert(obstacles[3].radius >= 0.5f * widest && obstacles[3].radius <= 0.75f * widest);
    }

    /*
    * The dither: the same for a cell every time, in 0..1, any range of thresholds taking its share of the cells, and
    * the cells under a small share apart (no two touching).
    */
    void testTheDither() {
        assert(ditherThreshold(3, -7) == ditherThreshold(3, -7));
        int under = 0;
        int inBand = 0;
        std::set<std::pair<int, int>> sparse;

        for (int row = -50; row < 50; ++row) {
            for (int column = -50; column < 50; ++column) {
                const float threshold = ditherThreshold(column, row);
                assert(threshold >= 0.0f && threshold < 1.0f);
                under += threshold < 0.3f ? 1 : 0;
                inBand += threshold >= 0.6f && threshold < 0.7f ? 1 : 0;

                if (threshold < 0.05f) {
                    sparse.insert({column, row});
                }
            }
        }

        assert(under >= 2950 && under <= 3050 && inBand >= 970 && inBand <= 1030);

        for (const auto& [column, row] : sparse) {
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    assert((dx == 0 && dz == 0) || !sparse.contains({column + dx, row + dz}));
                }
            }
        }
    }

    /*
    * A layer's share: its plants a hectare times a cell's area, none where bare, thinned or thickened in a gap by its
    * gap share, and by the path: all within nearPathMetres, farShare past farPathMetres.
    */
    void testALayersShare() {
        const ValleyMaps& maps = patchValleyMaps();
        PlantingLayer layer{.floorPerHectare = 100.0f, .footprintMetres = 5.0f, .gapShare = 0.5f, .nearPathMetres = 20.0f, .farPathMetres = 60.0f, .farShare = 0.4f};
        assert(pathDensityShare(layer, 10.0f) == 1.0f && near(pathDensityShare(layer, 40.0f), 0.7f, 1e-5f) && near(pathDensityShare(layer, 90.0f), 0.4f, 1e-5f));
        assert(layerShare(layer, maps, forestClearingCentre.x, forestClearingCentre.y) == 0.0f);

        // Find a place in the patch out of any gap and one fully in one.
        Vec2 open{1e9f, 0.0f};
        Vec2 gap{1e9f, 0.0f};

        for (float z = -200.0f; z < -50.0f; z += 1.0f) {
            for (float across = -100.0f; across <= 100.0f; across += 1.0f) {
                const float x = valleyAxis(z) + across;
                const float g = valleyGap(maps.settings, x, z);
                const float path = forestPathDistance(x, z);

                if (path < 20.0f && g == 0.0f) {
                    open = Vec2{x, z};
                }

                if (path < 20.0f && g == 1.0f) {
                    gap = Vec2{x, z};
                }
            }
        }

        assert(open.x < 1e8f && gap.x < 1e8f);
        const float floor = ecotope(valleyEcotopes(maps, open.x, open.y), Ecotope::Floor);
        assert(near(layerShare(layer, maps, open.x, open.y), floor * 100.0f * 25.0f / 10000.0f, 1e-4f));
        assert(near(layerShare(layer, maps, gap.x, gap.y), 0.5f * ecotope(valleyEcotopes(maps, gap.x, gap.y), Ecotope::Floor) * 0.25f, 1e-4f));

        // Never more than every cell.
        layer.floorPerHectare = 1e6f;
        assert(layerShare(layer, maps, open.x, open.y) == 1.0f);
    }

    /*
    * A layer's cells: about its share of the lattice's cells, each plant inside its cell's jitter and the region and
    * clear of the path, the same every run; one sharing its lattice never takes the same cell; none where it has none.
    */
    void testALayersCells() {
        const ValleyMaps& maps = patchValleyMaps();
        const PlantingRegion region{.northmost = -200.0f, .southmost = -50.0f, .spread = 100.0f};
        const PlantingLayer layer{.floorPerHectare = 120.0f, .footprintMetres = 4.0f, .jitterShare = 0.6f, .pathClearance = 3.0f};
        const std::vector<DitheredCell> cells = ditheredCells(layer, maps, region, 11u);
        float expected = 0.0f;

        for (float z = -200.0f; z < -50.0f; z += 4.0f) {
            for (float across = -100.0f; across < 100.0f; across += 4.0f) {
                const float x = valleyAxis(z + 2.0f) + across + 2.0f;
                expected += forestPathDistance(x, z + 2.0f) >= 3.0f ? layerShare(layer, maps, x, z + 2.0f) : 0.0f;
            }
        }

        assert(static_cast<float>(cells.size()) > 0.9f * expected && static_cast<float>(cells.size()) < 1.1f * expected);

        for (const DitheredCell& cell : cells) {
            const Vec2 middle{(static_cast<float>(cell.column) + 0.5f) * 4.0f, (static_cast<float>(cell.row) + 0.5f) * 4.0f};
            assert(std::fabs(cell.place.x - middle.x) <= 0.3f * 4.0f + 1e-4f && std::fabs(cell.place.y - middle.y) <= 0.3f * 4.0f + 1e-4f);
            assert(cell.place.y >= region.northmost && cell.place.y <= region.southmost);
            assert(std::fabs(cell.place.x - valleyAxis(cell.place.y)) <= region.spread);
            assert(forestPathDistance(cell.place.x, cell.place.y) >= layer.pathClearance);
            assert(cell.threshold == ditherThreshold(cell.column, cell.row) && cell.threshold < layerShare(layer, maps, middle.x, middle.y));
        }

        const std::vector<DitheredCell> again = ditheredCells(layer, maps, region, 11u);
        assert(again.size() == cells.size() && again.back().place.x == cells.back().place.x && again.back().firstDraw == cells.back().firstDraw);
        assert(ditheredCells(layer, maps, region, 12u).front().place.x != cells.front().place.x);

        // Sharing the lattice: the second takes the band above the first's share, never the same cell.
        const PlantingLayer second{.floorPerHectare = 300.0f, .footprintMetres = 4.0f, .jitterShare = 0.6f};
        const std::vector<DitheredCell> shared = ditheredCells(second, maps, region, 13u, &layer);
        std::set<std::pair<int, int>> taken;

        for (const DitheredCell& cell : cells) {
            taken.insert({cell.column, cell.row});
        }

        assert(!shared.empty());

        for (const DitheredCell& cell : shared) {
            assert(!taken.contains({cell.column, cell.row}));
        }

        const PlantingLayer otherLattice{.floorPerHectare = 300.0f, .footprintMetres = 5.0f};
        bool threw = false;

        try {
            ditheredCells(otherLattice, maps, region, 13u, &layer);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
        assert(ditheredCells(PlantingLayer{.footprintMetres = 4.0f}, maps, region, 11u).empty());
    }
}

void testValleyMaps() {
    testTheSettings();
    testTheLandsEcotopes();
    testTheMapsAreBuilt();
    testTheClearance();
    testTheCanopyCaps();
    testWhatFitsBlocksNoSight();
    testTheObstacles();
    testTheDither();
    testALayersShare();
    testALayersCells();
}
