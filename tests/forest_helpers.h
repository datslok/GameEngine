#pragma once

#include "assets/asset_manager.h"
#include "core/job_system.h"
#include "ecs/world.h"
#include "games/forest/forest_game.h"
#include "gameplay/control_mode.h"

/*
* A valley for the tests (forest_test, island_hangings_test, hanging_moss_test), so each does not grow its own copy.
* The engine hands a game its job system (Game::useJobs); without one a game builds on serialJobs(), one thread, and
* every test's valley (its ground, every pillar and island, their roots and moss) was carved on one core. One job
* system for the whole run serves them all; the valley is the same whatever order its jobs finish in (each writes its
* own slot), so the tests see what one thread would build.
*/
inline JobSystem& forestTestJobs() {
    static JobSystem jobs;
    return jobs;
}

/*
* A fresh valley, initialised headless with its stand-in ground (an asset folder that does not exist), its coarsest
* ground and fewer shapes by default, so the tests neither need the downloaded assets nor spend long building it.
*/
struct TestForest {
    World world;
    AssetManager assets;
    ForestGame game{ControlMode::FirstPerson, true, "no-such-folder/forest"};

    explicit TestForest(ForestValleySettings valley = ForestValleySettings{.groundCoarseness = 2, .pillarCount = 40, .islandCount = 8}) {
        game.getValleySettings() = valley;
        game.useJobs(forestTestJobs());
        game.onInit(world, assets);
    }
};
