#pragma once

#include "assets/image_loader.h"

#include <vector>

/*
* The valley's rock as one layer cake (ROADMAP phase 18, the pillars and floating islands): real sandstone pillars are
* what is left of a stack of flat beds eroded away, so the same bed stands at the same height on every one, and the eye
* joins the bands up across kilometres, a cue to the scale. Mostly one rock in slightly different tones, as real
* sandstone is (the first try, three different rocks in thin beds, read as a striped cake: the user's look, Nov 14 2026),
* with now and then a thin hard bed of blocks. Painted by world height (Material::groundPaint, GroundPaint::Strata) from
* valleyStrataBottom to valleyStrataTop. The same every run.
*/
inline constexpr float valleyStrataBottom = -400.0f;
inline constexpr float valleyStrataTop = 1400.0f;

/*
* One bed, from bottom to top (metres, world height): soft sandstone, its tone between the rock's two tints (0 the
* first, 1 the second; games/forest/forest_ground.h, forestRockLayers), or a hard bed of blocks, which will stand out
* as a ledge.
*/
struct StrataBand {
    float bottom = 0.0f;
    float top = 0.0f;
    bool hard = false;
    float tone = 0.0f;
};

/*
* The beds, bottom to top, touching, covering the cake: soft beds 15 to 60 m thick (mostly thinner), each tone at least
* 0.3 from the soft bed below so it shows; about one bed in five hard, 5 to 15 m thick, never two together.
*/
std::vector<StrataBand> valleyStrata();

// The bed holding a height (the lowest or highest beyond the cake's ends).
const StrataBand& strataBandAt(const std::vector<StrataBand>& bands, float height);

/*
* The strip of weights the material reads (texels wide, one tall, linear RGBA: the two sandstone tints, the blocks, and
* the moss, always 0): each texel's share of each is how much of its height span the beds cover, a soft bed's share
* split between the tints by its tone, so a bed's edge inside a texel blends and the shader's height blend sharpens it.
*/
ImageData strataImage(const std::vector<StrataBand>& bands, int texels);

/*
* The wavy beds' noise (Material::strataWarp): a square that repeats seamlessly, its red channel slow value noise of a
* few octaves about the middle (0.5), size texels across.
*/
ImageData strataWarpImage(int size);

/*
* The water streaks' texture (Material::groundStreaks): a seamless square, its red channel clean rock (1) with dark
* stains where noise runs high; the material stretches it down the cliffs into streaks.
*/
ImageData streakImage(int size);
