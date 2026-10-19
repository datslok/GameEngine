#ifndef RENDER_GPU_SHADER_SHARED_H
#define RENDER_GPU_SHADER_SHARED_H

/*
* Numbers the shaders and the C++ side must agree on, written once. C++ includes this file as usual; the shaders include
* it through glslc's #include support (the makefile passes -Isrc, so they write the same path as C++ does). Because GLSL
* reads it too, it may only hold #defines of plain numbers, behind an include guard rather than #pragma once. C++ code
* uses the typed constants built from these (in soft_shadow.h), not the macros.
*/

// Soft shadows: the narrowest and widest filter, and how far the blocker search reaches, in texels of a light's tile.
#define MIN_SHADOW_FILTER_TEXELS 1.0
#define MAX_SHADOW_FILTER_TEXELS 12.0
#define MAX_BLOCKER_SEARCH_TEXELS 12.0

// Sun shadows: the steepest slope (tangent of the angle between a surface's normal and the sun) the bias grows with.
#define MAX_SUN_BIAS_SLOPE 10.0

#endif
