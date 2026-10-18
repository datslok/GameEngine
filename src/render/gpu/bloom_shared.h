#ifndef RENDER_GPU_BLOOM_SHARED_H
#define RENDER_GPU_BLOOM_SHARED_H

/*
* Bloom's filter numbers, shared by render/gpu/bloom.cpp (the tested mirror) and the bloom shaders, so the two cannot
* drift apart. GLSL reads this file too, so it may only hold #defines of plain numbers.
*/

// The 13-tap downsample (Jimenez 2014), as five overlapping 4x4 boxes: the centre box (four reads one texel diagonally
// out) carries half the weight, the four corner boxes (built from a 5x5 grid of reads two texels apart) an eighth each.
// Each grid read belongs to one, two or four corner boxes, which gives its weight; all thirteen sum to 1.
#define BLOOM_INNER_BOX_WEIGHT 0.5
#define BLOOM_OUTER_BOX_WEIGHT 0.125

// The 3x3 tent the upsample spreads each texel with: corners 1, edges 2, centre 4, out of 16.
#define BLOOM_TENT_CORNER 0.0625
#define BLOOM_TENT_EDGE 0.125
#define BLOOM_TENT_CENTRE 0.25

// Brightness as the eye weighs the three primaries (Rec. 709 luminance), for the Karis average on the first downsample.
#define BLOOM_LUMA_RED 0.2126
#define BLOOM_LUMA_GREEN 0.7152
#define BLOOM_LUMA_BLUE 0.0722

// The chain stops before a level's shorter side would drop below this many texels: smaller levels hold too few texels
// to blur, and they add no reach (the blur is already as wide as the screen's shorter side).
#define BLOOM_SMALLEST_LEVEL_SIDE 4

// The most levels the settings may ask for. Eight halvings already reach past the screen at 8K.
#define BLOOM_LEVEL_LIMIT 8

#endif
