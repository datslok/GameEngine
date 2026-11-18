#pragma once

#include "core/settings_visitor.h"
#include "math/vec3.h"

/*
* How a layer's density changes with height. Exponential: falling by e every scaleHeight above baseHeight and growing
* below it (haze filling a valley). Pooled: the density throughout below baseHeight (its top), falling by e every
* scaleHeight above it (its soft edge): cold, damp air pooled on a valley's floor under warmer air, as jungle mist lies
* under the canopy.
*/
enum class FogShape { Exponential, Pooled };

/*
* Component: a layer of fog in the air near the ground (mist, haze), which the renderer lights with the frame's lights,
* so beams and lamps light the air round them and distant things fade into it (render/gpu/volumetric_fog.h). A game
* places them as it places an Atmosphere; the layers add (the first FOG_MAX_LAYERS count). Each fills the whole world
* by height (shape): its density is the light it takes out of a ray per metre (the extinction) at baseHeight. Of what
* it takes out, albedo scatters (per colour; the rest is absorbed): water droplets scatter nearly everything, smoke
* absorbs much. The scattered light goes mostly forward (anisotropy, the Henyey-Greenstein phase function's g), as
* droplets do, so the air glows round a lamp or the sun seen through it. Beyond maxDistance along a ray there is none of
* it, so the sky past it keeps showing.
*/
struct Fog {
    float density = 0.02f;      // extinction at baseHeight, per metre: 3 / density is about how far one sees through it
    float baseHeight = 0.0f;    // m: the exponential's base, or the pool's top
    float scaleHeight = 10.0f;  // m: the exponential's e-folding, or the pool's soft edge above its top
    Vec3 albedo{1.0f, 1.0f, 1.0f};
    float anisotropy = 0.6f;    // 0 even, towards 1 all forward
    float maxDistance = 1000.0f; // m along a ray
    FogShape shape = FogShape::Exponential;
};

void visit(Fog& fog, SettingsVisitor& visitor);
