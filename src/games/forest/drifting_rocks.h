#pragma once

#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "games/forest/valley_shapes.h"
#include "math/transform.h"
#include "math/vec3.h"

#include <cstdint>
#include <vector>

/*
* Rocks drifting round the floating islands and some pillars (ROADMAP phase 18): small islands of rock bobbing slowly
* up and down and turning, each at its own pace, so the valley's air moves. Their pose comes from the simulation's
* clock (as Spinner's does), never stored or integrated, so every run sees them in the same place at the same time.
* They cast shadows into the far shadow map too, so they throw moving shafts through the mist (the user's choice,
* Nov 14 2026), which redraws that map every frame.
*/

// A drifting rock (a component): where it floats at the middle of its bob, how big it is (its radius, metres), how far
// it rises and falls either way and over how long, where in the bob it starts, and its turn about the vertical.
struct DriftingRock {
    Vec3 rest;
    float size = 0.0f;
    float bobMetres = 0.0f;
    float bobSeconds = 1.0f;
    float phase = 0.0f;
    float turnRadians = 0.0f;
    float turnSpeed = 0.0f;
    std::uint32_t seed = 0;
};

// Its pose at a time: risen bobMetres x sin(2 pi t / bobSeconds + phase) above its rest, turned about the vertical by
// turnRadians + turnSpeed t, at its own size (scale 1: its mesh is built at its size).
Transform driftingRockPose(const DriftingRock& rock, double seconds);

// System: each drifting rock's Transform set to its pose at the simulation time, or at time 0 while they are still.
void updateDriftingRocks(World& world, double simulationSeconds, bool moving);

// How many drift and how (F5 Forest, Drifting rocks; start-up but whether they move, live, to compare their shadows' cost).
struct ForestDriftSettings {
    // Rocks round an island for each 10 m of its radius (an 80 m island: 8).
    float islandRocksPerTenMetres = 1.0f;

    // The share of pillars with one to three rocks.
    float pillarShare = 0.3f;

    // The largest rock's radius (metres); each is 10-100% of it, most small.
    float largestRock = 20.0f;

    // How far each rises and falls either way (metres), and the mean time of a rise and fall (each 50-150% of it).
    float bobMetres = 3.0f;
    float bobSeconds = 40.0f;

    // Off: they hold still, so the far shadow map is redrawn only when something else moves.
    bool moving = true;
};

void visit(ForestDriftSettings& settings, SettingsVisitor& visitor);

/*
* An island's rocks: islandRocksPerTenMetres for each 10 m of its radius, by dart throwing (each kept only if clear of
* those kept, by both sizes and both bobs), in a loose ring 1.35-1.95 radii out (clear of its jagged rim) from just
* under the rim to 0.6 radii below it. The same island always gives the same rocks.
*/
std::vector<DriftingRock> islandRocks(const IslandPlacement& island, const ForestDriftSettings& settings);

// A pillar's rocks: on pillarShare of pillars, one to three, 1.7-2.3 radii out (clear of its lean and bulges), at 45-90%
// of its height.
std::vector<DriftingRock> pillarRocks(const PillarPlacement& pillar, const ForestDriftSettings& settings);

/*
* The valley's rocks: every island's and pillar's, less those that would touch another shape (inside 1.6 radii of a
* pillar below its top, or 1.3 radii of an island between its tip and just over its rim).
*/
std::vector<DriftingRock> valleyRocks(const std::vector<PillarPlacement>& pillars, const std::vector<IslandPlacement>& islands,
                                      const ForestDriftSettings& settings);

// A rock's shape, in its own frame: an island's at its size, uncarved (no ledges, no joints, which at 2.5 m deep would
// cut a small rock apart).
SurfaceGrid driftingRockSurface(const DriftingRock& rock, std::uint32_t seed);
