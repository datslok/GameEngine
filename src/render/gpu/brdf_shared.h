#ifndef RENDER_GPU_BRDF_SHARED_H
#define RENDER_GPU_BRDF_SHARED_H

/*
* The physically based shading model's numbers, shared by render/gpu/brdf.cpp (the tested mirror) and triangle.frag, so
* the two cannot drift apart. GLSL reads this file too, so it may only hold #defines of plain numbers.
*/

// How much light a non-metal reflects head-on: ((n - 1) / (n + 1))^2 for a refractive index n of 1.5 (glass, plastic,
// stone, paint). glTF's metallic-roughness model fixes it at this value for every non-metal.
#define BRDF_DIELECTRIC_REFLECTANCE 0.04

// The smoothest surface the shader draws. A perfect mirror (roughness 0) would reflect a point light into a single
// infinitely bright point that no pixel can show; this keeps the sharpest highlight a few pixels wide (Filament's value).
#define BRDF_SMALLEST_ROUGHNESS 0.045

// The view's cosine is kept at least this far above zero: an eye exactly along the surface, or a normal interpolated
// across a curved mesh that points slightly away from the eye, would otherwise divide by zero.
#define BRDF_SMALLEST_VIEW_COSINE 1e-4

// Texels per side of the specular albedo table (render/gpu/brdf.h), over the view cosine and the roughness, 0..1
// each, with the first and last texels exactly at 0 and 1. The albedo changes smoothly, so a small table is enough.
#define BRDF_TABLE_SIZE 32

#endif
