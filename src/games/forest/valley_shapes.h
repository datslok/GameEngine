#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "assets/image_loader.h"
#include "games/forest/valley_strata.h"
#include "scene/indexed_mesh.h"
#include "scene/mesh.h"

#include <cstdint>
#include <functional>
#include <vector>

/*
* The valley's big shapes (ROADMAP phase 18): sandstone pillars and floating islands carved by the valley's beds, the
* Ring and the boxes the Monolith and the far towers are built from, all generated, and where they stand.
*/

/*
* A surface from a grid of points, rows up and columns around: point(row, column) for rows 0..rows and columns
* 0..columns - 1, the last column joined to the first, and the last row to the first too when wrapRows (a torus).
* Triangles turn counterclockwise seen from where "around" (increasing column) crossed with "up" (increasing row) points,
* so a surface going round anticlockwise seen from above with rows rising faces outwards. Triangles with no area (at a
* pole, where a row shrinks to one point) are left out; normals are smooth, each point's the area-weighted mean of its
* triangles', and texture coordinates run column / columns around and row / rows up.
*/
Mesh wrappedSurfaceMesh(int rows, int columns, bool wrapRows, const std::function<Vec3(int row, int column)>& point);

// A wrapped surface's points, (rows + 1) x columns of them (rows x columns when the rows wrap), row by row.
struct SurfaceGrid {
    int rows = 0;
    int columns = 0;
    bool wrapRows = false;
    std::vector<Vec3> points;
};

// The points of a wrapped surface, as wrappedSurfaceMesh takes them (std::invalid_argument for too few rows or columns).
SurfaceGrid surfaceGrid(int rows, int columns, bool wrapRows, const std::function<Vec3(int row, int column)>& point);

Mesh wrappedSurfaceMesh(const SurfaceGrid& grid);

/*
* The same surface straight in the render format (scene/indexed_mesh.h): one vertex a point, the same triangles and
* smooth normals, the tangent running round (increasing column), w +1 so up the picture is up the surface. Building a Mesh
* and converting it (resolving every corner, MikkTSpace, merging corners by hashing) cost several times as much, which
* the valley's 250 carved shapes, 3 million triangles, made the start's (and the tests') slowest part.
*/
IndexedMesh wrappedSurfaceIndexedMesh(const SurfaceGrid& grid);

// Where a pillar stands: its foot's centre (on the lowest ground under it, before sinking), its size and turn, and the
// seed of its own shape.
struct PillarPlacement {
    Vec3 foot;
    float radius = 0.0f;
    float height = 0.0f;
    float turnRadians = 0.0f;
    std::uint32_t seed = 0;
};

// Where an island floats: its rim's centre, its radius and turn, and the seed of its own shape.
struct IslandPlacement {
    Vec3 rim;
    float radius = 0.0f;
    float turnRadians = 0.0f;
    std::uint32_t seed = 0;
};

/*
* Vertical joints: real sandstone pillars split along near-vertical cracks into rough blocks (Zhangjiajie's look). Narrow
* grooves rockJointDepth deep about every rockJointSpacing metres round the rock, wandering slowly as they rise. How deep
* the joint at a place is (0..1 of the depth), from the angle round and the height in metres.
*/
inline constexpr float rockJointDepth = 2.5f;
inline constexpr float rockJointSpacing = 9.0f;
float jointGroove(float angleRadians, float height, float radius, std::uint32_t seed);

/*
* What carves the rock: the valley's beds (games/forest/valley_strata.h) at the heights the shader paints them, waved
* by the same noise, so a hard bed stands out exactly where its band shows on every pillar. The first shapes, eight
* unit meshes stretched to each pillar's size, could not: a bed's height in a shared mesh lands at a different world
* height on each pillar (and the stretched noise looked blobby: the user's look, Nov 14 2026).
*/
struct RockCarving {
    std::vector<StrataBand> bands;
    ImageData warp;
    float warpTileMetres = 1500.0f;
    float warpMetres = 6.0f;
    // How deep the joints are cut (rockJointDepth; 0 for a rock too small for them, as the drifting rocks).
    float jointMetres = rockJointDepth;
};

// How far the beds are moved up or down at a world place: the warp texture's red channel read as the GPU reads it
// (bilinear between texel centres, repeating every warpTileMetres), as warpedStrataHeight moves them.
float carvingWarp(const RockCarving& carving, float x, float z);

/*
* The rock's relief from its beds at a world height, in metres: a hard bed stands out rockLedgeMetres (it weathers
* slower), and the soft rock within rockUndercutReach under a hard bed is eaten back up to rockUndercutMetres, deepest
* right under it, so every ledge throws a shadow line.
*/
inline constexpr float rockLedgeMetres = 2.0f;
inline constexpr float rockUndercutMetres = 1.5f;
inline constexpr float rockUndercutReach = 4.0f;
float strataRelief(const std::vector<StrataBand>& bands, float height);

// The shapes' resolution in metres: a row every rockRowMetres up and a column every rockColumnMetres round.
inline constexpr float rockRowMetres = 2.5f;
inline constexpr float rockColumnMetres = 2.5f;

/*
* A sandstone pillar at its real size, in metres round its foot (not turned: its Transform turns it and places it): a
* column tapering by a third, leaning and bulging by noise, split by joints, its beds standing out and cut back, standing
* pillarBuriedDepth of its height into the ground, with a broken, nearly flat top. Its own shape from its seed.
*/
inline constexpr float pillarBuriedDepth = 0.05f;
SurfaceGrid pillarSurface(const PillarPlacement& pillar, const RockCarving& carving);

/*
* A floating island at its real size round its rim's centre (not turned): a jagged underside narrowing from the rim
* (y 0) down to a point islandDepth radii below, its beds and joints carved like a pillar's, and a gently domed top.
*/
inline constexpr float islandDepth = 1.4f;
SurfaceGrid islandSurface(const IslandPlacement& island, const RockCarving& carving);

/*
* A ring of unit radius standing on edge: a torus round the z axis, its tube tubeShare of the radius thick, in segments
* round the ring and sides round the tube.
*/
Mesh ringMesh(float tubeShare, int segments = 96, int sides = 16);

// How many pillars and islands the game makes (start-up values).
struct ValleyLayout {
    int pillarCount = 220;
    int islandCount = 30;
};

/*
* The pillars, by dart throwing (a Poisson disk: random places, each kept only if it stands far enough from all kept so
* far, so they never clump and leave no big gaps), over the valley and the land round it, 200-350 m tall and 12-45 m
* wide, the same every run. None stands near the path, the Ring, the clearing or the start. Fewer than asked if the
* tries run out.
*/
std::vector<PillarPlacement> placePillars(const ValleyLayout& layout);

/*
* The floating islands, the same way: 25-150 m in radius, their tips 380-700 m up, above every pillar's top, so they
* float clear over the valley (none over a pillar reaching within islandGap of its tip). None hangs over the start, so the first view up is open sky.
*/
std::vector<IslandPlacement> placeIslands(const ValleyLayout& layout, const std::vector<PillarPlacement>& pillars);

// A box: its centre and half its size along each axis (the engine's cube mesh is 2 m a side, scaled by halfSize).
struct ValleyBox {
    Vec3 centre;
    Vec3 halfSize;
};

/*
* The Monolith: a 600 m slab across the clearing's north end, with lower blocks either side, a crown on top, a deck
* standing out east, and a truss bridge west to a pillar on the rim (monolithPillar). The level is the clearing's height.
*/
std::vector<ValleyBox> monolithBoxes(float groundLevel);

// The pillar the Monolith's bridge reaches, west of it.
PillarPlacement monolithPillar(float groundLevel);

// The far towers: slabs 300-800 m tall standing 2.5-7 km away in the haze, with the ground under each.
std::vector<ValleyBox> farTowerBoxes();
