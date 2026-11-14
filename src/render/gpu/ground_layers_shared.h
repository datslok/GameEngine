#ifndef RENDER_GPU_GROUND_LAYERS_SHARED_H
#define RENDER_GPU_GROUND_LAYERS_SHARED_H

/*
* A layered material's switches and the projection's limit (render/gpu/ground_layers.h), shared by
* render/gpu/material_uniforms.cpp, ground_layers.cpp and the shaders, so they cannot drift apart. GLSL reads this file
* too, so it may only hold #defines of plain numbers.
*/

// The layers' textures projected along the object's own axes in metres (GpuMaterial::groundWeights[1], a flag).
#define GROUND_PROJECTED 1u

// The layers painted by a strip of weights by world height (strata) instead of a texture over the mesh's coordinates.
#define GROUND_STRATA 2u

// Each layer read by hex tiling (render/gpu/ground_layers.h, hexTiling): three taps at random offsets, so no repeat shows.
#define GROUND_HEX 4u

// Hex tiling's cells: the texture coordinates scaled by this before the triangle grid (Mikkelsen 2022's 2 sqrt 3), so
// a cell is a little under a third of a tile across.
#define GROUND_HEX_SCALE 3.4641016

// A hex tap's weight under this is dropped (and the rest scaled back up to one), so most places read one or two.
#define GROUND_HEX_SMALLEST 0.02

// A projection's share under this is dropped (and the rest scaled back up to one), so most pixels read one or two.
#define GROUND_PROJECTION_SMALLEST 0.1

#endif
