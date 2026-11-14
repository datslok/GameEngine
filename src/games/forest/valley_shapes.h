#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/mesh.h"

#include <cstdint>
#include <functional>
#include <vector>

/*
* The valley's big shapes (ROADMAP phase 18, the rough valley): sandstone pillars, floating islands, the Ring and the
* boxes the Monolith and the far towers are built from, all generated, and where they stand. Grey for now: the rough
* valley is a test of scale, haze and cost before any content.
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

/*
* A sandstone pillar of unit height (0 to 1, standing slightly into the ground below 0) and a base radius about 1: a
* column tapering by a third, leaning and bulging by noise, with ledges where its radius steps, and a rounded top. Each
* seed is a different pillar; the game scales them to their real size (tens of metres wide, hundreds tall).
*/
inline constexpr float pillarBuriedDepth = 0.05f;
Mesh pillarMesh(std::uint32_t seed, int rings = 40, int segments = 24);

/*
* A floating island of unit radius: a jagged underside narrowing from its rim (y 0) down to a point islandDepth below,
* and a gently domed top. Each seed is a different island.
*/
inline constexpr float islandDepth = 1.4f;
Mesh islandMesh(std::uint32_t seed, int rings = 28, int segments = 32);

/*
* A ring of unit radius standing on edge: a torus round the z axis, its tube tubeShare of the radius thick, in segments
* round the ring and sides round the tube.
*/
Mesh ringMesh(float tubeShare, int segments = 96, int sides = 16);

// Where a pillar stands: its foot's centre (on the lowest ground under it, before sinking), its size and turn, and which of the shapes.
struct PillarPlacement {
    Vec3 foot;
    float radius = 0.0f;
    float height = 0.0f;
    float turnRadians = 0.0f;
    int variant = 0;
};

// Where an island floats: its rim's centre, its radius and turn, and which of the shapes.
struct IslandPlacement {
    Vec3 rim;
    float radius = 0.0f;
    float turnRadians = 0.0f;
    int variant = 0;
};

// How many of each shape the game makes, and how the pillars and islands are spread (start-up values).
struct ValleyLayout {
    int pillarCount = 220;
    int islandCount = 30;
    int variants = 8;
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
