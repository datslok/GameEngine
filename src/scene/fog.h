#pragma once

#include "core/settings_visitor.h"
#include "math/vec3.h"

/*
* Component: fog in the air near the ground (mist, haze), which the renderer lights with the frame's lights, so beams
* and lamps light the air round them and distant things fade into it (render/gpu/volumetric_fog.h). A game places one,
* as it places an Atmosphere; the first counts. It fills the whole world, thinning upwards: its density is the light it
* takes out of a ray per metre (the extinction) at baseHeight, falling by e every scaleHeight above it and growing below.
* Of what it takes out, albedo scatters (per colour; the rest is absorbed): water droplets scatter nearly everything,
* smoke absorbs much. The scattered light goes mostly forward (anisotropy, the Henyey-Greenstein phase function's g), as
* droplets do, so the air glows round a lamp or the sun seen through it. Beyond maxDistance along a ray there is none,
* so the sky past it keeps showing.
*/
struct Fog {
    float density = 0.02f;      // extinction at baseHeight, per metre: 3 / density is about how far one sees through it
    float baseHeight = 0.0f;    // m
    float scaleHeight = 10.0f;  // m
    Vec3 albedo{1.0f, 1.0f, 1.0f};
    float anisotropy = 0.6f;    // 0 even, towards 1 all forward
    float maxDistance = 1000.0f; // m along a ray
};

void visit(Fog& fog, SettingsVisitor& visitor);
