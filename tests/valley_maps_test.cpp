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

    // The planting settings and every layer's fields, each named once, all read at start-up but the placed trees' switch
    // (live, off by default).
    void testTheSettings() {
        ForestPlantingSettings settings;
        FieldCounter counter;
        visit(settings, counter);
        assert(counter.fields == 13 && counter.live == 1 && !settings.placedTrees);

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
        const PlantingMaps& planting = maps.planting;
        assert(planting.columns == 120 && planting.rows == 120 && planting.inverseTexel == 1.0f / settings.texelMetres);
        assert(planting.ecotopes.size() == 120u * 120u && planting.clearance.size() == planting.ecotopes.size() && planting.caps.empty());
        assert(planting.gaps.size() == planting.ecotopes.size() && planting.path.size() == planting.ecotopes.size() && planting.axis.size() == 120u);
        assert(planting.pathPoints.size() == forestPathPoints().size() && planting.pathPoints.back().x == forestPathPoints().back().x);
        assert(planting.pathInverseLengths.size() + 1 == planting.pathPoints.size());
        assert(planting.texelReach >= 0.5f * std::sqrt(2.0f) * settings.texelMetres && planting.texelReach < settings.texelMetres);
        assert(planting.corridorClearance == settings.corridorClearance && planting.halfWidth == settings.halfWidth);

        for (const auto& texel : planting.ecotopes) {
            assert(texel[0] + texel[1] + texel[2] + texel[3] == 255);
        }

        const ValleyMaps serial = buildValleyMaps(settings, {}, {}, {}, serialJobs());
        assert(serial.planting.ecotopes == planting.ecotopes && serial.planting.clearance == planting.clearance);
        assert(serial.planting.gaps == planting.gaps && serial.planting.path == planting.path && serial.planting.axis == planting.axis);

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

            const float read = plantingClearance(maps.planting, x, z);
            const float reach = maps.planting.texelReach;
            assert(read <= truth + 1e-4f);

            if (truth < plantingFarthestClearance - 2.0f * reach) {
                assert(read >= truth - 2.0f * reach - 0.1f);
            } else {
                assert(read >= std::min(truth, plantingFarthestClearance) - 2.0f * reach - 0.1f);
            }
        }

        const ValleyMaps empty = buildValleyMaps(settings, {}, {}, {}, testJobs());
        assert(plantingClearance(empty.planting, valleyAxis(-150.0f), -150.0f) >= plantingFarthestClearance - empty.planting.texelReach - 0.1f);
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
        assert(maps.planting.caps.size() == 2);

        for (int i = 0; i < 400; ++i) {
            const float z = -280.0f + 200.0f * forestUnitRandom(7, 2 * i);
            const float x = 60.0f * (2.0f * forestUnitRandom(7, 2 * i + 1) - 1.0f);

            for (const float reach : {16.0f, 4.0f}) {
                const float lowest = lowestWithin(line, reach, x, z);
                const float cap = plantingCap(maps.planting, reach, x, z);
                assert(cap <= lowest + 1e-3f);

                if (lowest < 1e8f) {
                    assert(cap >= lowestWithin(line, reach + 3.0f * settings.texelMetres, x, z) - 2.0f);
                }
            }
        }

        // A smaller reach reads the smallest cap at least as far.
        assert(plantingCap(maps.planting, 3.0f, 0.0f, -200.0f) == plantingCap(maps.planting, 4.0f, 0.0f, -200.0f));
        assert(plantingCap(maps.planting, 4.0f, 50.0f, -200.0f) > 1e6f);
        assert(plantingCap(maps.planting, 4.0f, 0.0f, -2000.0f) > 1e6f);

        bool threw = false;

        try {
            plantingCap(maps.planting, 17.0f, 0.0f, -200.0f);
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

            const float capped = std::min(reach, maps.planting.caps.back().reach);

            if (!plantingFitsTheMaps(maps.planting, Vec2{x, z}, capped, centre + reach)) {
                ++refused;
                continue;
            }

            ++fitted;
            assert(!blocksSight(Vec3{x, centre, z}, capped, lines));

            for (const ValleyObstacle& obstacle : obstacles) {
                assert((Vec2{x, z} - obstacle.centre).length() >= obstacle.radius + capped - 1e-3f);
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
    * A layer's share: its plants a hectare times a cell's area, none where bare, thinned or thickened in a gap by its
    * gap share, and by the path: all within nearPathMetres, farShare past farPathMetres.
    */
    void testALayersShare() {
        const ValleyMaps& maps = patchValleyMaps();
        const PlantingLayer asked{.floorPerHectare = 100.0f, .footprintMetres = 5.0f, .gapShare = 0.5f, .nearPathMetres = 20.0f, .farPathMetres = 60.0f, .farShare = 0.4f};
        PlantingLayerRecord layer;
        setPlantingDensity(layer, plantingDensity(asked));
        assert(plantingLayerShare(layer, maps.planting, forestClearingCentre.x, forestClearingCentre.y) == 0.0f);

        // A place in the patch by the path out of any gap, and one fully in one.
        Vec2 open{1e9f, 0.0f};
        Vec2 gap{1e9f, 0.0f};

        for (float z = -200.0f; z < -50.0f; z += 1.0f) {
            for (float across = -100.0f; across <= 100.0f; across += 1.0f) {
                const float x = valleyAxis(z) + across;
                const float g = plantingGap(maps.planting, x, z);
                const float path = plantingPathDistanceAt(maps.planting, x, z);

                if (path < 20.0f && g == 0.0f) {
                    open = Vec2{x, z};
                }

                if (path < 20.0f && g == 255.0f) {
                    gap = Vec2{x, z};
                }
            }
        }

        assert(open.x < 1e8f && gap.x < 1e8f);
        const float floor = ecotope(valleyEcotopes(maps, open.x, open.y), Ecotope::Floor);
        assert(near(plantingLayerShare(layer, maps.planting, open.x, open.y), floor * 100.0f * 25.0f / 10000.0f, 1e-4f));
        assert(near(plantingLayerShare(layer, maps.planting, gap.x, gap.y), 0.5f * ecotope(valleyEcotopes(maps, gap.x, gap.y), Ecotope::Floor) * 0.25f, 1e-4f));

        // Never more than every cell.
        PlantingLayer every = asked;
        every.floorPerHectare = 1e6f;
        setPlantingDensity(layer, plantingDensity(every));
        assert(plantingLayerShare(layer, maps.planting, open.x, open.y) == 1.0f);
    }

    /*
    * The maps read without the CPU's sines and square roots: the axis between its rows within a millimetre of
    * valleyAxis, the gaps within 0.02 of the noise nearly everywhere and 0.15 anywhere (it turns from none to a full gap
    * over a few metres, measured at most 0.094), the path's distance (the density's blend only)
    * within a few centimetres of forestPathDistance nearly everywhere and 1.5 m anywhere (it bends within a texel on
    * the centre line, at the corners and along the ridge inside each bend); a place clears the path exactly as
    * forestPathDistance says.
    */
    void testTheAxisGapsAndPath() {
        const ValleyMaps& maps = patchValleyMaps();
        int nearPath = 0;
        int blurred = 0;
        int strayGaps = 0;

        for (int i = 0; i < 4000; ++i) {
            const float z = -400.0f + 480.0f * forestUnitRandom(13, 3 * i);
            const float x = valleyAxis(z) + 300.0f * (2.0f * forestUnitRandom(13, 3 * i + 1) - 1.0f);
            assert(std::fabs(plantingAxisAt(maps.planting, z) - valleyAxis(z)) < 1e-3f);
            const float gapError = std::fabs(plantingGap(maps.planting, x, z) / 255.0f - valleyGap(maps.settings, x, z));
            assert(gapError < 0.15f);
            strayGaps += gapError >= 0.02f ? 1 : 0;

            const float truth = forestPathDistance(x, z);

            if (truth < 60.0f) {
                const float error = std::fabs(plantingPathDistanceAt(maps.planting, x, z) - truth);
                assert(error < 1.5f);
                ++nearPath;
                blurred += error >= 0.05f ? 1 : 0;
            }

            const float clearance = 4.0f * forestUnitRandom(13, 3 * i + 2);

            if (std::fabs(truth - clearance) > 1e-3f) {
                assert(plantingClearOfThePath(maps.planting, Vec2{x, z}, clearance) == (truth >= clearance));
            }
        }

        assert(nearPath > 500 && blurred * 20 < nearPath && strayGaps * 20 < 4000);
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
    testALayersShare();
    testTheAxisGapsAndPath();
}
