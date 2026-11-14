#pragma once

#include "core/job_system.h"

/*
* One job system for the whole test run. The engine hands a game its job system (Game::useJobs); without one a game
* builds on serialJobs(), one thread, so a test that builds a big game (the forest's valley: its ground, every pillar
* and island, their roots and moss, its trees) carves it all on one core. Every test that builds a game gives it this
* one. What a game builds is the same whatever order its jobs finish in (each writes its own slot), so the tests see what
* one thread would build.
*/
inline JobSystem& testJobs() {
    static JobSystem jobs;
    return jobs;
}
