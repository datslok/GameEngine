#pragma once

#include "core/job_system.h"
#include "core/settings_visitor.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/asset_handles.h"
#include "scene/frame_description.h"
#include "scene/material.h"
#include "scene/mesh.h"
#include "scene/terrain_heights.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

/*
* Terrain level of detail (CDLOD, Strugar 2010; docs/LOD.md, Terrain): the land as a quadtree of square nodes over its
* heights, each drawn as one small grid mesh the vertex shaders lift onto the land (terrain.glsl). Each frame the nodes
* are chosen round the camera, finer near it, each level reaching twice as far as the last, and every vertex slides
* smoothly into the next coarser level's grid towards the end of its level's range, so the levels meet without cracks
* and change without popping.
*/

// The ranges (RenderSettings::terrain, F5 Terrain): how far the finest level reaches (each level after reaches twice
// as far), and the share of each level's band, at its far end, over which its points slide into the next level's.
struct TerrainSettings {
    float firstRangeMetres = 200.0f;
    float morphShare = 0.3f;
};

void visit(TerrainSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a range not above 0 or a share outside 0..1 (exclusive of 0).
void validateTerrain(const TerrainSettings& settings);

// A square the terrain's material covers (its weights texture across it, as the texture coordinates are): a node takes
// the finest region holding it whole, else the last.
struct TerrainRegion {
    Vec2 centre;
    float halfSize = 0.0f;
    Material material;
};

// Each node's lowest and highest height, a level at a time from the leaves (level 0) to the root, nodes row by row.
struct TerrainBounds {
    std::vector<std::vector<float>> lowest;
    std::vector<std::vector<float>> highest;
};

/*
* Component: a terrain a game places (its entity needs no Transform: the heights are where they are). The quadtree's
* root is leafSize * 2^(levels - 1) across round centre; nodes wholly outside landHalfSize of it are not drawn. grid
* and halfGrid are terrainGridMesh(TERRAIN_GRID_QUADS) and terrainGridMesh(TERRAIN_HALF_GRID_QUADS), added by the game.
* heightsVersion changes whenever heights do, so the renderer uploads them again.
*/
struct Terrain {
    std::shared_ptr<const TerrainHeights> heights;
    std::uint64_t heightsVersion = 1;
    std::shared_ptr<const TerrainBounds> bounds;
    Vec2 centre;
    float leafSize = 64.0f;
    int levels = 10;
    float landHalfSize = 0.0f;
    std::vector<TerrainRegion> regions;
    MeshHandle grid;
    MeshHandle halfGrid;
};

// The bounds of a terrain's nodes from its heights: every grid point in a leaf and its corners' heights, then each
// parent the extremes of its children (a row of leaves a job).
TerrainBounds makeTerrainBounds(const Terrain& terrain, JobSystem& jobs);

// A flat grid of quads x quads squares over x and z from 0 to 1, facing up, UVs its x and z: a node's mesh.
Mesh terrainGridMesh(int quads);

// How far a level reaches: firstRangeMetres doubled each level.
float terrainRange(int level, const TerrainSettings& settings);

// One node to draw: its square, its level, whether it is a quarter of a node drawn at that node's level (the half
// grid), its grid's spacing, its morph's start and end, and its lowest and highest height.
struct TerrainNode {
    Vec2 corner;
    float size = 0.0f;
    int level = 0;
    bool half = false;
    float spacing = 0.0f;
    float morphStart = 0.0f;
    float morphEnd = 0.0f;
    float lowest = 0.0f;
    float highest = 0.0f;
};

// A node of a level at a corner, its spacing and morph (the level's range end, back morphShare of its band).
TerrainNode terrainNodeAt(int level, const Vec2& corner, float size, bool half, const TerrainSettings& settings);

/*
* The nodes to draw this frame (Strugar's selection): from the root down, a node out of its own level's reach is left to
* its parent, which draws that quarter at its level; one the next finer level's range reaches at all (its nearest point)
* is split; the rest are drawn whole. Every place on the land lies in exactly one, and neighbours differ by a level at most.
*/
std::vector<TerrainNode> selectTerrainNodes(const Terrain& terrain, const Vec3& eye, const TerrainSettings& settings);

/*
* The terrain's draws, appended: a node each, its grid mesh placed by its model matrix (corner, size across x and z),
* its bounds round the node's box, still, casting shadows, its region's material, and its patch (appended to patches,
* DrawItem::terrainPatch its index). The nodes are selectTerrainNodes', chosen before the frame was built so it kept
* room for them (buildFrame's laterDraws): reserving here would move every draw already in the list.
*/
void appendTerrainDraws(const Terrain& terrain, std::span<const TerrainNode> nodes, std::vector<DrawItem>& draws,
                        std::vector<TerrainPatch>& patches);
