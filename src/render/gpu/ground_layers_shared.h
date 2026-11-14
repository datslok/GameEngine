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

// A projection's share under this is dropped (and the rest scaled back up to one), so most pixels read one or two.
#define GROUND_PROJECTION_SMALLEST 0.1

#endif
