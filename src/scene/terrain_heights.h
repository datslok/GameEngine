#pragma once

#include "core/job_system.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/terrain_shared.h"

#include <cstdint>
#include <functional>
#include <vector>

/*
* The terrain's heights and its patches' vertices (CDLOD, Strugar 2010; docs/LOD.md, Terrain): the maths the vertex
* shaders run (terrain.glsl mirrors every function here), so the patches the GPU draws can be tested on the CPU.
*/

// A square of the land's heights: size points a side, spacing metres apart, from corner (the least x and z), rows
// along x (point row * size + column is at corner + (column, row) * spacing).
struct TerrainHeightGrid {
    Vec2 corner;
    float spacing = 1.0f;
    std::uint32_t size = 0;
    std::vector<float> heights;
};

// The grids, finest first (at most TERRAIN_MOST_GRIDS): a place reads the finest holding it.
struct TerrainHeights {
    std::vector<TerrainHeightGrid> grids;
};

// A grid of height(x, z) over the square centre +- halfSize, spacing apart (halfSize a whole number of spacings), a row a job.
TerrainHeightGrid makeTerrainHeightGrid(const Vec2& centre, float halfSize, float spacing, const std::function<float(float, float)>& height,
                                        JobSystem& jobs);

// The land's height at a place: bilinear in the finest grid holding it; past every grid, the coarsest's nearest edge.
float terrainHeight(const TerrainHeights& heights, float x, float z);

// The land's unit normal at a place, from its slope over step metres either way.
Vec3 terrainNormal(const TerrainHeights& heights, float x, float z, float step);

// Bilinear between four heights: the near row (left, right), the far row, across and down in 0..1.
float terrainBilinear(float nearLeft, float nearRight, float farLeft, float farRight, float across, float down);

// The height on a drawn grid square (the same four heights, across and down in 0..1): the plane through the triangle
// holding the place, the square split as terrainGridMesh splits it, from its near right corner to its far left.
float terrainTriangleHeight(float nearLeft, float nearRight, float farLeft, float farRight, float across, float down);

/*
* The drawn land's height at a place: as the finest patches draw it near the camera (their grid on the finest grid's
* points, each square two flat triangles), in the finest grid holding it; past every grid, the coarsest's nearest edge.
* What meshes following the ground stand on (scene/ground_follow.h).
*/
float terrainSurfaceHeight(const TerrainHeights& heights, float x, float z);

// How far a point is through its patch's morph: 0 before start, 1 from end, linear between.
float terrainMorph(float distance, float start, float end);

// A grid point slid by the morph: an odd coordinate moves towards its even neighbour below, reaching it at morph 1, so
// a fully morphed patch is the next coarser level's grid.
Vec2 morphGridCoordinate(const Vec2& grid, float morph);

/*
* One patch the shaders draw (GpuTerrainPatch on the GPU): the node's square (corner, size), its grid's quads and their
* spacing, the morph's start and end (metres from the eye), and the texture coordinates across its region's square
* (corner, and how far one metre goes).
*/
struct TerrainPatch {
    Vec2 corner;
    float size = 0.0f;
    float quads = 0.0f;
    float spacing = 0.0f;
    float morphStart = 0.0f;
    float morphEnd = 0.0f;
    Vec2 uvCorner;
    float uvPerMetre = 0.0f;
};

// A patch's vertex as the shaders make it.
struct TerrainVertex {
    Vec3 position;
    Vec3 normal;
    Vec3 tangent;
    float tangentSign = -1.0f;
    Vec2 uv;
};

/*
* The vertex at a grid mesh's point (its x and z, 0 to 1 across): the grid point on the node, its distance from the eye
* at the land's height there giving the morph, slid by it, and lifted to the land's height where it lands. Its normal
* is the slope's over the spacing, growing to the next level's as it morphs, so the shading follows the shape. The
* tangent runs along x in the surface (u grows with x); the bitangent is the other side of it, as the ground's rings had it
* (v grows with z, south, down the picture).
*/
TerrainVertex terrainVertex(const TerrainHeights& heights, const TerrainPatch& patch, const Vec2& meshPoint, const Vec3& eye);
