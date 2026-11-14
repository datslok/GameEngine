#pragma once

#include "assets/image_loader.h"
#include "core/job_system.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/mesh.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/*
* The valley's land (games/forest, ROADMAP phase 18): a valley kilometres long running north (-z), its floor at height 0
* between walls rising to a rim 150 m up, its southern end climbing as a ramp to the rim where the walk starts, mountains
* beyond the rim, and the small hills and lumps of noise over all of it. A path runs from the start down the ramp,
* through the Ring and along the floor to a level clearing at the Monolith's foot. Everything here is a pure function of
* the place, so the drawn ground, the ground walked on and the paint over it agree, and tests can ask any point. North
* is -z, east +x, as in the other games.
*
* The sizes are the concept art's (the plates of the valley from its rim, the Ring and the Monolith's foot): the
* Monolith about 1 km from the rim, so it fills two thirds of the view there.
*/
inline constexpr float valleyRimHeight = 150.0f;
inline constexpr float valleyFloorHalfWidth = 200.0f; // the floor's flat part either side of the valley's axis
inline constexpr float valleyWallWidth = 150.0f;      // how far the walls take to climb to the rim

// The ramp: the floor climbing from rampStart to the rim at rampEnd, southwards.
inline constexpr float valleyRampStart = 50.0f;
inline constexpr float valleyRampEnd = 750.0f;

// The start, on the rim at the ramp's top, looking north down the valley.
inline const Vec2 forestStartPlace{0.0f, 790.0f};

// The Ring: standing on edge across the path, facing along it (its axis is z), sunk to its waist.
inline const Vec2 forestRingPlace{0.0f, 0.0f};
inline constexpr float forestRingRadius = 200.0f;
inline constexpr float forestRingTubeRadius = 12.0f;

// The Monolith: its front face's centre, facing south, and the level clearing before it.
inline const Vec2 forestMonolithPlace{0.0f, -400.0f};
inline const Vec2 forestClearingCentre{0.0f, -330.0f};
inline constexpr float forestClearingRadius = 120.0f;
inline constexpr float forestClearingBlend = 60.0f;

// The path: level across within its half width, blending back into the land over the next forestPathBlend metres, and
// sunk a little into it, as a trodden path is.
inline constexpr float forestPathHalfWidth = 1.5f;
inline constexpr float forestPathBlend = 3.0f;
inline constexpr float forestPathSink = 0.08f;

// The meadow round the start, on the rim: open grass within this radius.
inline constexpr float forestMeadowRadius = 60.0f;

// The ground's layers, one channel of the weights texture each.
inline constexpr std::size_t forestGrassLayer = 0;
inline constexpr std::size_t forestLitterLayer = 1;
inline constexpr std::size_t forestMudLayer = 2;
inline constexpr std::size_t forestRockLayer = 3;

// Smooth value noise (0..1) on a unit lattice: hashed lattice values blended with a quintic fade, so its slope is
// continuous too. The same place and seed always give the same value.
float forestValueNoise(float x, float z, std::uint32_t seed);

// Octaves of value noise, each twice as fine and half as strong as the last and turned its own way (so the sum has no
// square grain), centred on 0 (-1..1).
float forestFractalNoise(float x, float z, int octaves, std::uint32_t seed);

// The path's centre line, from the start to the clearing (x, z points).
const std::vector<Vec2>& forestPathPoints();

// How far a place is from the path's centre line, across the ground.
float forestPathDistance(float x, float z);

// The valley's axis: the x of its middle at a place along it (straight where the walk is, winding further north).
float valleyAxis(float z);

// The ground's height at a place, in metres (0 on the valley's floor, about valleyRimHeight on the rim).
float forestHeight(float x, float z);

// The ground's unit normal at a place, from the height's slope over step metres either way.
Vec3 forestNormal(float x, float z, float step = 0.5f);

// How open the canopy is at a place: 1 for meadow and clearing grass, 0 under the forest.
float forestMeadowShare(float x, float z);

// How much of each layer covers a place (grass, leaf litter, mud, mossy rock), adding up to one; the slope is measured
// over normalStep metres, the size of what the paint can show.
std::array<float, 4> forestGroundPaint(float x, float z, float normalStep = 0.5f);

/*
* A square of the land a material covers: its weights texture and its texture coordinates span it, 0 to 1 west to east
* and north to south.
*/
struct GroundExtent {
    Vec2 centre;
    float halfSize = 0.0f;
};

float forestTextureU(float x, const GroundExtent& extent);
float forestTextureV(float z, const GroundExtent& extent);

// Where a texel's centre lies across an extent, for a texture of `size` texels a side (x for columns, z for rows).
float forestTexelCentreX(int texel, int size, const GroundExtent& extent);
float forestTexelCentreZ(int texel, int size, const GroundExtent& extent);

/*
* The land as nested square rings round one centre, each with squares coarser than the one inside, so a few hundred
* thousand squares reach 20 km: what is far away covers few pixels, so it needs few squares. A ring's squares are
* spacing metres; its outer border follows the next ring's coarser squares exactly (outerSpacing), so the two meet with
* no cracks. The first ring has no hole (innerHalfSize 0).
*/
struct TerrainRing {
    float innerHalfSize = 0.0f;
    float outerHalfSize = 0.0f;
    float spacing = 0.0f;
    float outerSpacing = 0.0f; // the next ring's spacing, or spacing for the last ring
};

// The land's centre: halfway along the walk, so the finest ring covers all of it.
inline const Vec2 forestTerrainCentre{0.0f, 170.0f};

// The finest ring's half size; each ring after is four times larger, the last half as large again (20 km).
inline constexpr float forestNearHalfSize = 640.0f;

/*
* The rings for the finest squares' size (2, 4 or 8 m: the halves must hold whole squares of every ring): the finest
* spacing within 640 m, then 8 times coarser to 2.56 km, 32 times to 10.24 km, 128 times to 20.48 km. Throws
* std::invalid_argument for another spacing.
*/
std::vector<TerrainRing> forestTerrainRings(float nearSpacing);

// The extent of everything the rings cover, and of the finest ring alone.
GroundExtent forestFarExtent();
GroundExtent forestNearExtent();

// A ring's height at a place: the land's, except along its outer border, where it follows the next ring's coarser edge.
float forestRingHeight(float x, float z, const TerrainRing& ring);

/*
* One ring as a mesh round forestTerrainCentre: its squares at their heights, smooth normals (measured over its own
* spacing), texture coordinates across extent. Rows run north to south, columns west to east; triangles face up. Its
* rows are measured on jobs' threads, each into its own slots, so the mesh is the same on any number of them.
*/
Mesh forestTerrainRingMesh(const TerrainRing& ring, const GroundExtent& extent, JobSystem& jobs = serialJobs());

// The paint across an extent as a weights texture of size x size texels (RGBA: grass, litter, mud, rock), linear bytes,
// a row a job.
ImageData forestWeightsImage(int size, const GroundExtent& extent, JobSystem& jobs = serialJobs());
