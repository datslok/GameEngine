#ifndef RENDER_GPU_SHADER_SHARED_H
#define RENDER_GPU_SHADER_SHARED_H

/*
* Numbers the shaders and the C++ side must agree on, written once. C++ includes this file as usual; the shaders include
* it through glslc's #include support (the makefile passes -Isrc, so they write the same path as C++ does). Because GLSL
* reads it too, it may only hold #defines of plain numbers, behind an include guard rather than #pragma once. C++ code
* uses the typed constants built from these (maxPointLights in light_uniforms.h, and so on), not the macros.
*/

// The most lights of each kind the shader holds: the sizes of the arrays in triangle.frag's LightData block.
#define MAX_DIRECTIONAL_LIGHTS 4
#define MAX_POINT_LIGHTS 64
#define MAX_SPOT_LIGHTS 8

// Soft shadows: the narrowest and widest filter, and how far the blocker search reaches, in texels of a light's tile.
#define MIN_SHADOW_FILTER_TEXELS 1.0
#define MAX_SHADOW_FILTER_TEXELS 12.0
#define MAX_BLOCKER_SEARCH_TEXELS 12.0

#endif
