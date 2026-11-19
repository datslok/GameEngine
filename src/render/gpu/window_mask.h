#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "render/gpu/window_mask_shared.h"

#include <cstdint>

/*
* Slit windows on a building's walls without geometry (ROADMAP phase 18, the structures, stage 4; docs/research/
* STRUCTURES.md, "Lights at night"), mirrored in assets/shaders/window_mask.glsl. A wall's place in its facet's plane
* (metres across, and the height) falls in rows of window cells pitch wide; a band (Material::windowBands, a strip over
* height) says how much of each height a row of slits covers, what share of them is lit, what share is the cool colour
* and how much of each cell the slit fills across. Each window's state comes from a hash of its cell and its face.
*
* Far away a slit is smaller than a pixel. Drawn as a hard edge it would flicker and TAA would wipe it out, so the mask
* is box-filtered: across, the slits' exact coverage of the pixel's footprint (pulseCoverage); up, the band strip's mips;
* and once a pixel spans more than a cell the windows' own states fade to their expected value (share lit, the colours'
* mix, mean brightness 1), so a distant band glows steadily at its true average.
*/
inline constexpr float windowRowHeight = static_cast<float>(WINDOW_ROW_HEIGHT);
inline constexpr float windowFarStart = static_cast<float>(WINDOW_FAR_START);
inline constexpr float windowFarFull = static_cast<float>(WINDOW_FAR_FULL);

// How much of [0, x] periodic slits cover: each pitch-wide cell holds one slit, length long, in its middle.
float pulseIntegral(float x, float pitch, float length);

// How much of a pixel footprint wide around centre the slits cover (0..1); a footprint of (nearly) none samples the point.
float pulseCoverage(float centre, float footprint, float pitch, float length);

// A face's seed from its facet numbers (each rounded to WINDOW_SEED_STEPS).
std::uint32_t windowFaceSeed(Vec2 facetNumbers);

// A window cell's three random numbers (0..1): lit, colour, brightness; from its column, its row and its face.
Vec3 windowHash(float across, float height, float pitch, std::uint32_t faceSeed);

// How far the windows have faded to their average (0 their own states, 1 the average), from the pixel's footprint.
float windowFarShare(Vec2 footprint, float pitch);

/*
* The light a wall's pixel gives off: place (across, height) and footprint (metres a pixel, across and up), the band there
* (x the rows' cover, y the share lit, z the share in the cool colour, w the slit's share of its cell across), the cells'
* pitch, the face's seed and the two colours (linear, brightness included). Zero outside the bands.
*/
Vec3 windowEmission(Vec2 place, Vec2 footprint, const Vec4& band, float pitch, std::uint32_t faceSeed, const Vec3& warm, const Vec3& cool);
