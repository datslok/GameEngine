#pragma once

#include "assets/image_loader.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/mesh.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/*
* The forest's land (games/forest): a 400 m square of rolling hills and hollows made from noise, with a level clearing
* at the origin (where the structure will stand) and a path winding south from it to a meadow. Everything here is a pure
* function of the place, so the drawn ground, the ground walked on and the paint over it agree, and tests can ask any
* point. North is -z, east +x, as in the other games.
*/
inline constexpr float forestHalfSize = 200.0f;
inline constexpr int forestGridSquares = 400; // 1 m squares

// The clearing: level within this radius, blending back into the hills over the next forestClearingBlend metres.
inline constexpr float forestClearingRadius = 25.0f;
inline constexpr float forestClearingBlend = 18.0f;

// The path: level across within its half width, blending back into the land over the next forestPathBlend metres, and
// sunk a little into it, as a trodden path is.
inline constexpr float forestPathHalfWidth = 1.2f;
inline constexpr float forestPathBlend = 2.5f;
inline constexpr float forestPathSink = 0.08f;

// The meadow at the path's southern end: open grass within this radius.
inline const Vec2 forestMeadowCentre{30.0f, 150.0f};
inline constexpr float forestMeadowRadius = 32.0f;

// The ground's layers, one channel of the weights texture each.
inline constexpr std::size_t forestGrassLayer = 0;
inline constexpr std::size_t forestLitterLayer = 1;
inline constexpr std::size_t forestMudLayer = 2;
inline constexpr std::size_t forestRockLayer = 3;

// Smooth value noise (0..1) on a unit lattice: hashed lattice values blended with a quintic fade, so its slope is
// continuous too. The same place and seed always give the same value.
float forestValueNoise(float x, float z, std::uint32_t seed);

// Octaves of value noise, each twice as fine and half as strong as the last, centred on 0 (-1..1).
float forestFractalNoise(float x, float z, int octaves, std::uint32_t seed);

// The path's centre line, from the clearing's centre to the meadow's (x, z points).
const std::vector<Vec2>& forestPathPoints();

// How far a place is from the path's centre line, across the ground.
float forestPathDistance(float x, float z);

// The ground's height at a place, in metres (0 at the clearing).
float forestHeight(float x, float z);

// The ground's unit normal at a place, from the height's slope.
Vec3 forestNormal(float x, float z);

// How open the canopy is at a place: 1 for meadow and clearing grass, 0 under the forest.
float forestMeadowShare(float x, float z);

// How much of each layer covers a place (grass, leaf litter, mud, mossy rock), adding up to one.
std::array<float, 4> forestGroundPaint(float x, float z);

// The ground's texture coordinates for a place: 0 to 1 across the land, west to east and north to south.
float forestTextureU(float x);
float forestTextureV(float z);

// Where a texel's centre lies across the land, for a texture of `size` texels a side.
float forestTexelCentre(int texel, int size);

// The ground as a mesh: squares x squares squares over the land at its heights, with smooth normals and its texture coordinates.
Mesh forestTerrainMesh(int squares = forestGridSquares);

// The paint as a weights texture of size x size texels (RGBA: grass, litter, mud, rock), linear bytes.
ImageData forestWeightsImage(int size);
