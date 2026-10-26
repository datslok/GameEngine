#pragma once

#include "cloth/cloth.h"

/*
* The default fabric with the tethers off: for tests that measure a solver itself (XPBD, vertex block descent, the
* coarse levels), whose stretch the tethers would otherwise hold at their slack whatever the solver does.
*/
inline ClothSettings untetheredSettings() {
    ClothSettings settings;
    settings.tethers = false;
    return settings;
}
