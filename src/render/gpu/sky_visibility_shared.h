#ifndef SKY_VISIBILITY_SHARED_H
#define SKY_VISIBILITY_SHARED_H

// Numbers the sky visibility grid's C++ (render/gpu/sky_visibility.h) and its shaders (sky_visibility.glsl) share.

// Each cascade's points along x and z, and up y.
#define SKY_VISIBILITY_ACROSS 48
#define SKY_VISIBILITY_HIGH 24

// What a point the grid knows nothing about reads (not baked yet, or past the last cascade): an open field, the sky
// above and the ground below, its bands a = 1/2 and b = 3/4 straight up. A floor then gets the whole flat ambient, a
// wall and the air half, as they do in the open inside the grid (less the ground's bounce, which is not known there).
#define SKY_OPEN_FIELD_OPEN 0.5
#define SKY_OPEN_FIELD_LEAN_UP 0.75

// The most cascades the frame's records hold.
#define SKY_VISIBILITY_MAX_CASCADES 4

// A point whose rays meet the back of a surface more often than this share is inside a wall: left out when read.
#define SKY_VISIBILITY_BURIED_SHARE 0.25

// The steps a point's reach along an axis is stored in (8 bits: 255 is a whole spacing or more).
#define SKY_VISIBILITY_REACH_STEPS 255

// The texels of four 32-bit words each grid point takes along x in the shaders' image (GpuSkyCell).
#define SKY_CELL_TEXELS 3

// A point's state, in its record's top byte: not baked yet, baked, or inside a wall.
#define SKY_VISIBILITY_UNKNOWN 0
#define SKY_VISIBILITY_BAKED 1
#define SKY_VISIBILITY_BURIED 2

#endif
