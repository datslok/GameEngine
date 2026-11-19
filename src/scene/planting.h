#pragma once

#include "math/vec2.h"
#include "scene/planting_shared.h"

#include <array>
#include <cstdint>

/*
* Planting from maps, the maths both sides run (ROADMAP phase 18, the valley's stage 2, docs/research/VALLEY.md): the
* CPU here, the GPU in planting.glsl, tested to give the same bits. Every function is adds, multiplies, compares and whole
* numbers only, with any reciprocal or turn passed in from the CPU: no division, square root or trigonometry, which a GPU
* may round differently. Shaders keep their copies `precise`, so no multiply and add is fused into one rounding (the CPU
* build targets plain x86-64, which has no fused instruction, and its planting files say -ffp-contract=off as well).
* Reading the maps and composing a layer: scene/planting_maps.h.
*/

inline constexpr float plantingUnitStep = static_cast<float>(PLANTING_UNIT_STEP);
inline constexpr float plantingFullTurn = static_cast<float>(PLANTING_FULL_TURN);
inline constexpr float plantingDecimetre = static_cast<float>(PLANTING_DECIMETRE);
inline constexpr float plantingCentimetre = static_cast<float>(PLANTING_CENTIMETRE);
inline constexpr int plantingMostKinds = PLANTING_MOST_KINDS;
inline constexpr int plantingMostSharedKinds = PLANTING_MOST_SHARED_KINDS;
inline constexpr std::uint64_t plantingDrawsPerCell = PLANTING_DRAWS_PER_CELL;
inline constexpr std::uint64_t plantingDrawsUsed = PLANTING_DRAWS_USED;

// A random number in 0..1 (24 bits) for a seed and an index: SplitMix64's finaliser twice, the same on every machine.
float plantingRandom(std::uint64_t seed, std::uint64_t index);

// A lattice cell as one number, for its random numbers: its column's 32 bits over its row's.
std::uint64_t plantingCellKey(int column, int row);

/*
* The ordered dither's threshold of a lattice cell (0..1): Roberts' R2 sequence over the cells, fract(column a1 + row a2)
* with a1, a2 from the plastic number, in 32-bit whole numbers. Any range of thresholds is spread evenly over the cells,
* so the cells under a share are about that share of them and far apart where it is small.
*/
float plantingThreshold(int column, int row);

// A straight blend from a to b, share of the way (0..1).
float plantingBetween(float a, float b, float share);

// A place on a map's grid: how many texels it lies past the grid's origin.
float plantingCoordinate(float position, float origin, float inverseTexel);

/*
* A layer's share of its density at a distance from the path: 1 within near, farShare past far, a straight blend between
* (inverseSpan is 1 / (far - near), or anything where they meet).
*/
float plantingPathShare(float distance, float near, float far, float farShare, float inverseSpan);

/*
* The share of a layer's dither cells planted at a place (0..1): the floor's, wall's and rim's blended weights (0..255
* each) times the layer's share of a cell a unit of weight, its gap there (0..255) times gapStep thickening or thinning
* it, then the path's share; none where the weights give none.
*/
float plantingShare(const std::array<float, 3>& weights, const std::array<float, 3>& sharePerWeight, float gap, float gapStep, float pathShare);

/*
* Whether a cell is planted: its threshold in the band from below (the share of a layer sharing the lattice, planted
* first) to below plus its own share (no more than what is left of 1).
*/
bool plantingTaken(float threshold, float below, float share);

// A cell's plant's place: the cell's middle, moved by its two random numbers (0..1) up to half of jitterMetres each way.
Vec2 plantingJitteredPlace(int column, int row, float footprint, float jitterMetres, float drawX, float drawZ);

// A plant's scale and turn from its random numbers.
float plantingScale(float least, float range, float draw);
float plantingTurn(float draw);

// A kind from a random number: one of count alike, or by cumulative shares (the share up to each kind but the last).
int plantingUniformKind(float draw, int count);
int plantingSharedKind(float draw, const std::array<float, plantingMostSharedKinds - 1>& cumulative, int count);

// Whether two places on the ground are closer than radius.
bool plantingWithin(const Vec2& place, const Vec2& other, float radius);

// A stored whole number in metres: times its step, less an offset.
float plantingStored(float stored, float step, float offset);

// A path distance in metres from the map's blended centimetres.
float plantingPathDistance(float blended);

/*
* Whether a place is closer than radius to a straight piece of a path from start along along (inverseLengthSquared is
* 1 / |along|^2, made on the CPU): the nearest point on the piece, compared squared.
*/
bool plantingNearSegment(const Vec2& place, const Vec2& start, const Vec2& along, float inverseLengthSquared, float radius);

// The k-th point of the ring round a trunk reach away, a turn of k eighths from +x towards +z.
Vec2 plantingRingPoint(const Vec2& place, float reach, int k);

// A plant's top: its foot's height, its sphere's centre that far up a unit of scale, and its reach above that.
float plantingTop(float footHeight, float centrePerScale, float scale, float reach);

// Whether a plant fits the maps: its reach clear of every obstacle, its top corridor under every sight line's cap.
bool plantingFits(float clearance, float reach, float top, float corridor, float cap);

/*
* Whether a plant gives way to a neighbour of its own layer: the neighbour comes first (a lower threshold, or the same
* and an earlier cell) and stands closer than spacingShare of the larger reach.
*/
bool plantingGivesWay(const Vec2& place, float reach, float threshold, const Vec2& other, float otherReach, float otherThreshold,
                      bool otherEarlier, float spacingShare);
