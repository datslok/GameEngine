#ifndef SKY_VISIBILITY_SHARED_H
#define SKY_VISIBILITY_SHARED_H

// Numbers the sky visibility grid's C++ (render/gpu/sky_visibility.h) and its shaders (sky_visibility.glsl) share.

// Each cascade's points along x and z, and up y.
#define SKY_VISIBILITY_ACROSS 48
#define SKY_VISIBILITY_HIGH 24

// The most cascades the frame's records hold.
#define SKY_VISIBILITY_MAX_CASCADES 4

// A point whose rays meet the back of a surface more often than this share is inside a wall: left out when read.
#define SKY_VISIBILITY_BURIED_SHARE 0.25

// The steps a point's reach along an axis is stored in (8 bits: 255 is a whole spacing or more).
#define SKY_VISIBILITY_REACH_STEPS 255

// A point's state, in its record's top byte: not baked yet, baked, or inside a wall.
#define SKY_VISIBILITY_UNKNOWN 0
#define SKY_VISIBILITY_BAKED 1
#define SKY_VISIBILITY_BURIED 2

#endif
