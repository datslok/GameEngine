#ifndef CORE_DITHER_SHARED_H
#define CORE_DITHER_SHARED_H

/*
* The dither's strength, shared by core/dither.cpp and the shader that dithers (triangle.frag, which includes this file
* through glslc's #include support), so the tested C++ mirror and the shader cannot drift apart. GLSL reads it too, so
* it may only hold #defines of plain numbers.
*
* The noise moves a colour by up to half of one 8-bit sRGB step either way: kept just under a full step, so a value
* exactly on a level can never be pushed onto the rounding boundary.
*/
#define DITHER_STEP_FRACTION 0.99
#define DITHER_LEVELS 255.0

#endif
