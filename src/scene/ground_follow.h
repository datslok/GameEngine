#pragma once

#include "math/vec3.h"
#include "scene/frame_description.h"
#include "scene/terrain_heights.h"

#include <span>

/*
* Meshes that follow the ground (ModelRenderer::followsGround; docs/GROUNDCOVER.md): a patch of grass or ground cover
* is built standing on y = 0 of its own frame, and every vertex shader lifts each vertex by the land's height at the
* vertex's own place in the world (terrainHeight, terrain_data.glsl), as the terrain lifts its own flat patches. Over a
* blade's few centimetres of lean that is its root's height, so a few shared patches cover every lumpy spot of land
* instead of a mesh each (docs/GROUNDCOVER.md: about 1.2 GB for the forest's meadow and clearing).
*
* The CPU never moves the vertices; it lifts what it reasons with, the spheres views cull by and levels are chosen by.
*/

// A placed point lifted onto the land: raised by the land's height at its x and z (the vertex shaders' lift).
Vec3 liftedOntoGround(const TerrainHeights& heights, const Vec3& placed);

/*
* A sphere covering everything in sphere lifted onto the land: the land's lowest and highest heights under the sphere's
* square (read at its edges and at every line of the finest grid holding its centre between them, where bilinear land
* has its extremes) move it up by their middle and grow it by half their range. An unknown size (radius below 0) stays
* unknown.
*/
BoundingSphere liftedSphere(const BoundingSphere& sphere, const TerrainHeights& heights);

/*
* Each draw following the ground has its bounds and its levels' sphere lifted (liftedSphere); the others are left alone,
* and without land (no heights) so is everything, as the shaders lift by nothing then. Run once on draws whose spheres
* were just made from their meshes (the engine's attach step), never twice on the same spheres.
*/
void liftGroundDraws(std::span<DrawItem> draws, const TerrainHeights* heights);
