#pragma once

#include "core/settings_visitor.h"

/*
* Component: the wind over the land, which the renderer's wind force field (render/gpu/wind.h) turns into the sway of
* everything growing. A game places one, as it places a Fog; the first counts. It blows towards towardsDegrees (from +x
* towards +z, about up) at speed, its speed rising and falling by up to gustStrength of it in gusts about gustSize across
* that travel with it, and pushing sideways by up to turbulence of it in eddies about turbulenceSize across. Under a
* closed rainforest canopy the wind is under 1 m/s; above it a few m/s (docs/WIND.md).
*/
struct Wind {
    float towardsDegrees = 0.0f;
    float speed = 3.0f;          // m/s
    float gustStrength = 0.6f;   // a share of the speed
    float gustSize = 40.0f;      // m
    float turbulence = 0.3f;     // a share of the speed, sideways
    float turbulenceSize = 10.0f; // m
};

void visit(Wind& wind, SettingsVisitor& visitor);
