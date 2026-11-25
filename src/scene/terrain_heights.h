#pragma once

#include "core/job_system.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "scene/terrain_shared.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

/*
* The terrain's heights and its patches' vertices (CDLOD, Strugar 2010; docs/research/LOD.md, Terrain): the maths the vertex
* shaders run (terrain.glsl mirrors every function here), so the patches the GPU draws can be tested on the CPU.
*/

/*
* A square of the land's heights: size points a side, spacing metres apart, from corner (the least x and z), rows
* along x (point row * size + column is at corner + (column, row) * spacing). With its min/max pyramid (as Far Cry 5's
* GPU terrain and CDLOD bound their patches; docs/research/VIRTUALSHADOWMAPS.md, Terrain clusters' bounds): level 1
* the lowest and highest of each 2 x 2 of points, each level above of the 2 x 2 below (an odd side's last texel takes
* what is left), up to one texel; levels one after another, rows along x, a (lowest, highest) pair a texel.
*/
struct TerrainHeightGrid {
    Vec2 corner;
    float spacing = 1.0f;
    std::uint32_t size = 0;
    std::vector<float> heights;
    std::vector<float> pyramid;
};

// The pyramid's levels above the points (the last one texel across; 0 for a grid of one point), a level's texels a side
// (level 0: the points, size), and where a level's pairs start (level 1 at 0).
int terrainPyramidLevels(std::uint32_t size);
std::uint32_t terrainPyramidLevelSize(std::uint32_t size, int level);
std::size_t terrainPyramidLevelStart(std::uint32_t size, int level);

// A grid's pyramid from its heights (makeTerrainHeightGrid builds it; a grid whose heights change builds it again), a
// level at a time, its rows shared over the jobs. Cost grows with the grid's points.
std::vector<float> terrainHeightPyramid(const TerrainHeightGrid& grid, JobSystem& jobs);

// The lowest and highest the land stands somewhere.
struct TerrainHeightRange {
    float lowest = 0.0f;
    float highest = 0.0f;
};

// The lowest and highest under a texel of a level (level 0: the point itself).
TerrainHeightRange terrainPyramidTexel(const TerrainHeightGrid& grid, int level, std::uint32_t column, std::uint32_t row);

// The grids, finest first (at most TERRAIN_MOST_GRIDS): a place reads the finest holding it.
struct TerrainHeights {
    std::vector<TerrainHeightGrid> grids;
};

// A grid of height(x, z) over the square centre +- halfSize, spacing apart (halfSize a whole number of spacings), a row a job.
TerrainHeightGrid makeTerrainHeightGrid(const Vec2& centre, float halfSize, float spacing, const std::function<float(float, float)>& height,
                                        JobSystem& jobs);

// The land's height at a place: bilinear in the finest grid holding it; past every grid, the coarsest's nearest edge.
float terrainHeight(const TerrainHeights& heights, float x, float z);

/*
* Every height terrainHeight or terrainSurfaceHeight can give in the box (x west to east, z north to south): the
* heights of every grid's points round the squares the box touches, one point wider each way (clamped onto each grid,
* so a box past a grid reads its edge), since both lie between their square's corners, and a hair more for rounding.
* Every grid's from the finest up to the first holding the whole box (no coarser one is read there). Those points read
* from the pyramid: the level where they span at most 2 x 2 texels, four texels a grid, so a little wider, never short.
* Mirrored by terrain_heights.glsl (terrainHeightRange). Cost: four reads a grid, whatever the box's size. None
* without grids: 0. Throws std::invalid_argument for a grid without its pyramid.
*/
TerrainHeightRange terrainHeightRange(const TerrainHeights& heights, float west, float east, float north, float south);

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

/*
* Where a cluster of a patch's grid can stand on the land (ROADMAP phase 21 4c-viii), as (west, north, east, south): its
* footprint (the mesh's x and z, 0 to 1 across: low x, low z, high x, high z) as the patch's grid points, rounded out,
* one point more towards the corner (the morph slides an odd point down onto its even neighbour, never up), on the
* patch. Every vertex terrainVertex makes of the cluster lies in it, whatever the eye. Mirrored by terrain.glsl.
*/
Vec4 terrainClusterBox(const TerrainPatch& patch, const Vec4& footprint);

// The sphere round a box on the land (west, north, east, south) between two heights, a hair larger for rounding at the
// box's distance from the origin (terrainBoxSphere, mirrored by terrain.glsl).
Vec4 terrainBoxSphere(const Vec4& box, const TerrainHeightRange& heights);
