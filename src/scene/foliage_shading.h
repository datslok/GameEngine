#pragma once

#include "math/vec3.h"
#include "scene/foliage_shared.h"

#include <cstdint>

/*
* How foliage is shaded (ROADMAP phase 21 stage 3b-iii-c): the normals that light a crown as clumps, and each leaf's own
* tone. Mirrored by foliage_shading.glsl, which the twigs' vertex and fragment shaders use; the forest's cards bake the
* same in on the CPU (games/forest/forest_trees, leaf_clusters).
*/

inline constexpr float foliageFacingShare = static_cast<float>(FOLIAGE_FACING_SHARE);
inline constexpr float foliageLeastFrontShare = static_cast<float>(FOLIAGE_LEAST_FRONT_SHARE);
inline constexpr float leafDarkestTone = static_cast<float>(LEAF_DARKEST_TONE);
inline constexpr float leafToneRange = static_cast<float>(LEAF_TONE_RANGE);
inline constexpr float leafMostWarmth = static_cast<float>(LEAF_MOST_WARMTH);
inline constexpr std::uint32_t leafToneSteps = LEAF_TONE_STEPS;
inline constexpr std::uint32_t leafToneSeeds = LEAF_TONE_SEEDS;
static_assert(leafToneSteps * leafToneSteps == leafToneSeeds, "A seed is a brightness and a warmth");

/*
* A leaf's facing bent by its lean (the clump's and crown's ways out, the same for every leaf of a card or twig): the
* facing times its share plus the lean, made unit, never leaning less than foliageLeastFrontShare to the facing.
*/
Vec3 bentFoliageNormal(const Vec3& facing, const Vec3& lean);

/*
* A modelled leaf's normal (a unit facing) between its own (blend 0) and bent by its twig's lean as the cards' were
* (blend 1), made unit: the twigs' "Leaf normal blend".
*/
Vec3 blendedFoliageNormal(const Vec3& normal, const Vec3& lean, float blend);

// A twig leaf's tone seed (0..LEAF_TONE_SEEDS - 1) from its plant (its pick's bits: trees of one look share their twigs'
// numbers), its twig's number in its look and its leaf's number in the twig.
std::uint32_t leafToneSeed(std::uint32_t plant, std::uint32_t twig, std::uint32_t leaf);

// A seed's brightness and warmth (its low steps and its high steps, each spread evenly over the cards' ranges).
float leafTone(std::uint32_t seed);
float leafWarmth(std::uint32_t seed);

// What a leaf's linear colour is multiplied by: its tone and warmth on the sRGB colour, as the cards bake them, taken to
// linear light.
Vec3 leafToneScale(std::uint32_t seed);

/*
* A leaf's texture coordinate across carries a number in its whole part (the texture repeats, so it reads the same
* texels): a twig's prototype numbers its leaves (numberedLeafU, u in 0..1), and the vertex shader swaps that number for
* the leaf's tone seed with its twig (tonedLeafU), which the fragment shader reads back (leafToneSeedOf).
*/
float numberedLeafU(float u, std::uint32_t leaf);
float tonedLeafU(float numberedU, std::uint32_t plant, std::uint32_t twig);
std::uint32_t leafToneSeedOf(float u);
