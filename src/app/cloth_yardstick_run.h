#pragma once

#include "app/game_registry.h"

#include <ostream>

/*
* The cloth yardstick (cloth/cloth_yardstick.h), run by --cloth-yardstick: every rung settled at the default fabric's
* substeps and at four times as many (what the same solver gives nearer convergence), each as a row of its stretch,
* sag, how settled it is and its cost per tick: on the CPU on every core, then on the GPU (render/vulkan/vulkan_cloth.h)
* with its dispatches a tick, when there is one. Run it on the release build (make yardstick): the cost is what the
* later solvers are compared with. Then the dress-sized test: the cloth lab's dress on its mannequin cut at four
* spacings, held by contact alone and pinned at the shoulders, with XPBD, each row its stretch (mean, 99th percentile,
* worst), seams, hem height, nearest approach to the body, settling and cost. The quick run (make yardstick-quick)
* settles three sheets and three dresses for less time at the fabric's substeps on the CPU alone, to judge a change in
* about a minute; its numbers are not the recorded ones. The dress run (make yardstick-dress) is the dress-sized test
* alone, as the full run has it.
*/
void runClothYardstick(std::ostream& out, YardstickRun run);
