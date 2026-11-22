#pragma once

#include "core/settings_visitor.h"
#include "render/gpu/parallax_shared.h"

/*
* Switches for what materials may draw (F5 Materials, engine.render.materials.*), each on by default: off draws every
* material as if it had none of it, for comparing.
*/
struct MaterialSettings {
    // Clear coats (render/gpu/brdf.h, addClearCoat): the varnish layer materials may have over them (car paint, lacquer).
    bool clearCoat = true;

    // Parallax occlusion mapping (render/gpu/parallax.h): surfaces with a height map seem to stand out of their flat
    // polygons, each pixel showing the bump its view ray meets first.
    bool parallax = true;

    // The bumps' shadows on themselves from the brightest directional light (the sun), found through the same height map.
    bool parallaxSelfShadows = true;

    // The most steps a pixel's search takes, at grazing views (looking straight down it takes PARALLAX_FEWEST_STEPS):
    // fewer is cheaper, with stair steps showing on steep bumps seen side-on.
    int parallaxSteps = 32;

    // Light through leaves (render/gpu/brdf.h, addDiffuseTransmission): a leaf lit from behind glows with the light its
    // far side receives.
    bool lightThroughLeaves = true;

    // Twigs' modelled leaves (scene/foliage_shading.h, blendedFoliageNormal): their normals from their own (0) to bent
    // by their clump and crown as the cards' were (1, the look the cards were tuned to), so a crown reads as lit clumps.
    float leafNormalBlend = 1.0f;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(MaterialSettings& settings, SettingsVisitor& visitor);
