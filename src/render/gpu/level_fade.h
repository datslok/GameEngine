#pragma once

#include "render/gpu/instance_shared.h"

#include <cstdint>

/*
* Levels cross-faded by a dither (docs/research/LOD.md, Hiding the switches): over a band of distance before a draw's
* switch, both its levels are drawn, each keeping the pixels the other throws away by one noise turned each frame, which
* temporal anti-aliasing and the upscalers blend. A draw list entry carries the level's fade field. Mirrored by
* level_fade.glsl.
*/

inline constexpr std::uint32_t drawListFadeShift = DRAW_LIST_FADE_SHIFT;
inline constexpr std::uint32_t drawListFadeMask = DRAW_LIST_FADE_MASK;
inline constexpr std::uint32_t drawListInstanceMask = DRAW_LIST_INSTANCE_MASK;
inline constexpr std::uint32_t drawListFadeCoarse = DRAW_LIST_FADE_COARSE;
inline constexpr std::uint32_t drawListFadeSteps = DRAW_LIST_FADE_STEPS;

// The most instances a draw list entry can name.
inline constexpr std::uint64_t drawListInstances = std::uint64_t{DRAW_LIST_INSTANCE_MASK} + 1;

// An entry's fade field (0: not fading).
std::uint32_t drawListFade(std::uint32_t entry);

// The field of a level drawn while its draw fades: share 0 to 1 across the band, coarse for the coarser level. Never 0.
std::uint32_t levelFadeField(float share, bool coarse);

// The noise below which the coarser level keeps a pixel: the middle of the field's step.
float levelFadeThreshold(std::uint32_t field);

// Whether the level keeps a pixel of noise 0..1: the coarser below the threshold, the finer from it; not fading, all.
bool levelFadeKeeps(std::uint32_t field, float noise);

// The pixel's noise for the fade, turned each frame (frame below 1024).
float levelFadeNoise(float pixelX, float pixelY, std::uint32_t frame);
