#ifndef RENDER_GPU_NORMAL_MAPPING_SHARED_H
#define RENDER_GPU_NORMAL_MAPPING_SHARED_H

/*
* Normal mapping's numbers, shared by render/gpu/normal_mapping.cpp (the tested mirror) and normal_mapping.glsl, so the
* two cannot drift apart. GLSL reads this file too, so it may only hold #defines of plain numbers.
*/

// How far below 1 an unfiltered normal map texel's length can fall from 8-bit rounding alone: each decoded component is
// off by at most 1/255, which shortens a unit direction by at most (|x| + |y| + |z|) / 255 <= sqrt(3) / 255. Toksvig
// filtering counts only the shortening beyond this as bumps averaged away, so a sharp texture's own texels add nothing.
#define NORMAL_TOKSVIG_QUANTIZATION_SLACK 0.0068

#endif
