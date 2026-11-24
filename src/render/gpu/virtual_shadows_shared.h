#ifndef RENDER_GPU_VIRTUAL_SHADOWS_SHARED_H
#define RENDER_GPU_VIRTUAL_SHADOWS_SHARED_H

// The sun's virtual shadow maps (virtual_shadows.glsl). Only #defines of plain numbers, since GLSL includes this file
// too; C++ reads them as typed constants in render/gpu/virtual_shadows.h.

// A page's side in texels (Unreal's, Frogfood's, StratusGFX's): 64 KB of 32-bit depth.
#define VIRTUAL_SHADOW_PAGE_TEXELS 128

// The most levels a clipmap can have: from 0.5 m, the 16th reaches 16 km.
#define VIRTUAL_SHADOW_MAX_LEVELS 16

// How many pages a point is kept from its level's edge (the level choice's margin): the level slides by whole pages, so
// the camera sits up to half a page off its centre, and the other half page (64 texels) holds a soft shadow's widest
// filter and blocker search (24 texels) inside the level.
#define VIRTUAL_SHADOW_MARGIN_PAGES 1

// The level of a point no level holds: past the last one, where the far shadow map answers.
#define VIRTUAL_SHADOW_NO_LEVEL -1

#endif
