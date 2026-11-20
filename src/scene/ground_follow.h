#pragma once

#include "math/vec3.h"
#include "scene/frame_description.h"
#include "scene/terrain_heights.h"

#include <span>

/*
* Meshes that follow the ground (ModelRenderer::followsGround; docs/research/GROUNDCOVER.md): a patch of grass or ground cover
* is built standing on y = 0 of its own frame, and every vertex shader lifts each vertex by the land's height at the
* vertex's own place in the world as the land is drawn there (terrainSurfaceHeight, terrain_data.glsl), as the terrain lifts its own flat patches. Over a
* blade's few centimetres of lean that is its root's height, so a few shared patches cover every lumpy spot of land
* instead of a mesh each (docs/research/GROUNDCOVER.md: about 1.2 GB for the forest's meadow and clearing).
*
* The CPU never moves the vertices; it lifts what it reasons with, the spheres views cull by and levels are chosen by.
*/

/*
* A placed point lifted onto the land: raised by the drawn land's height at its x and z (terrainSurfaceHeight) less the
* height its object stands at (its matrix's origin: 0 for an entity's patch, the land under a placed plant's foot), the
* vertex shaders' lift (groundLift in terrain_data.glsl).
*/
Vec3 liftedOntoGround(const TerrainHeights& heights, const Vec3& placed, float standing = 0.0f);

/*
* A sphere covering everything in sphere lifted onto the land by an object standing at standing: the drawn land's lowest
* and highest heights under the sphere's square (read at its corners, every grid point of the finest grid holding its
* centre inside it, and along its edges wherever they cross a grid line or a square's diagonal: the drawn triangles'
* extremes all lie there) move it up by their middle less standing and grow it by half their range. An unknown size
* (radius below 0) stays unknown.
*/
BoundingSphere liftedSphere(const BoundingSphere& sphere, const TerrainHeights& heights, float standing = 0.0f);

/*
* Each draw following the ground has its bounds and its levels' sphere lifted (liftedSphere, standing at its matrix's
* origin); the others are left alone, and without land (no heights) so is everything, as the shaders lift by nothing
* then. Run once on draws whose spheres were just made from their meshes (the engine's attach step), never twice on the
* same spheres. Planted parts are not these draws: their spheres are in the plant's frame (placed_drawing.h).
*/
void liftGroundDraws(std::span<DrawItem> draws, const TerrainHeights* heights);
