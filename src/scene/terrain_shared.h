#ifndef SCENE_TERRAIN_SHARED_H
#define SCENE_TERRAIN_SHARED_H

// The terrain's patches (scene/terrain.h and its GPU twin, terrain.glsl). Only #defines of plain numbers, since GLSL
// includes this file too.

// A node's grid: quads a side of the full grid, and of the half grid that draws a quarter of a node at its own level.
#define TERRAIN_GRID_QUADS 32
#define TERRAIN_HALF_GRID_QUADS 16

// The most height grids a terrain has (finest first).
#define TERRAIN_MOST_GRIDS 4

#endif
