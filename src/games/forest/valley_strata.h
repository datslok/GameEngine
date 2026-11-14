#pragma once

#include "assets/image_loader.h"

#include <array>
#include <vector>

/*
* The valley's rock as one layer cake (ROADMAP phase 18, the pillars and floating islands): real sandstone pillars are
* what is left of a stack of flat beds eroded away, so the same bed stands at the same height on every one, and the eye
* joins the bands up across kilometres, a cue to the scale. One list of bands from strataBottom to strataTop, each one
* of three rocks (games/forest/forest_ground.h, forestRockLayers: red-brown strata, grey layers, big orange blocks),
* painted by world height (Material::groundPaint, GroundPaint::Strata). The same every run.
*/
inline constexpr float valleyStrataBottom = -400.0f;
inline constexpr float valleyStrataTop = 1400.0f;

// How many rocks the bands choose from (the rock layers before the moss on top).
inline constexpr int valleyStrataRocks = 3;

// One bed: from bottom to top (metres, world height), and which rock (0..valleyStrataRocks - 1).
struct StrataBand {
    float bottom = 0.0f;
    float top = 0.0f;
    int rock = 0;
};

/*
* The beds, bottom to top, touching, covering valleyStrataBottom to valleyStrataTop: 4 to 30 m thick (mostly thin, as
* the square of a random number is mostly small), no two neighbours the same rock, the red-brown strata half the time.
*/
std::vector<StrataBand> valleyStrata();

// The rock at a height: the bed holding it (the lowest or highest bed beyond the cake's ends).
int strataRockAt(const std::vector<StrataBand>& bands, float height);

/*
* The strip of weights the material reads (texels wide, one tall, linear RGBA, a channel a rock and alpha, the moss,
* always 0): each texel's share of each rock is how much of its height span that rock's beds cover, so a bed's edge
* inside a texel blends, and the shader's height blend sharpens it along the rocks' own shapes.
*/
ImageData strataImage(const std::vector<StrataBand>& bands, int texels);
