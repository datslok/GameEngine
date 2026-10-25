#pragma once

#include <ostream>

/*
* The cloth yardstick (cloth/cloth_yardstick.h), run by --cloth-yardstick: every rung settled at the default fabric's
* substeps and at four times as many (what the same solver gives nearer convergence), each as a row of its stretch,
* sag, how settled it is and its cost per tick on every core. Run it on the release build (make yardstick): the cost
* is what the later solvers are compared with.
*/
void runClothYardstick(std::ostream& out);
