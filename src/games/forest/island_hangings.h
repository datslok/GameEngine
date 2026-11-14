#pragma once

#include "core/settings_visitor.h"
#include "games/forest/valley_shapes.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"

#include <cstdint>
#include <vector>

/*
* What hangs from the floating islands (ROADMAP phase 18): roots and vines grown as curves from each island's underside,
* drooping under their own weight, swept into tubes of bark, the vines carrying ivy leaves on cut-out cards. Built once
* at start-up in the island's own frame (the rock's: its rim at the origin, turned with it), kept only in the render
* format as the rock is.
*/

// The metres between a strand's points: each ring of its tube.
inline constexpr float hangingStepMetres = 1.0f;

// A hanging curve: its points from the anchor down, a step apart at most, and its radius at each.
struct HangingStrand {
    std::vector<Vec3> points;
    std::vector<float> radii;
};

/*
* A strand of length metres from anchor: it leaves leaning out (outward, made level), turns down within a few metres
* as a heavy rope does (the share pointing down 1 - exp(-s / bend) at s metres along), wandering a little sideways by
* smooth noise of its own seed, never climbing; its radius falls from startRadius to tipRadius, fast near the tip.
*/
HangingStrand hangingStrand(const Vec3& anchor, const Vec3& outward, float length, float startRadius, float tipRadius, std::uint32_t seed);

/*
* Lobes standing out round a tube near the ground (a tree's buttress roots): count fins, each at its own angle (evenly
* spread, jittered by the seed) and its own height (60-100% of amplitude, a share of the radius), falling away by e every
* heightMetres above groundHeight (full below it), each as narrow as cos^sharpness. No lobes when count is 0.
*/
struct TubeLobes {
    int count = 0;
    float amplitude = 0.0f;
    float heightMetres = 1.0f;
    float sharpness = 8.0f;
    float groundHeight = 0.0f;
    std::uint32_t seed = 0;
};

/*
* The radius of a lobed tube at an angle round it (radians from its frame's first axis) and a height:
* r (1 + exp(-max(height - ground, 0) / h) sum_j a_j max(0, cos(angle - angle_j))^p).
*/
float lobedRadius(float radius, float angle, float height, const TubeLobes& lobes);

// How a tube is drawn beyond its shape: its bark in whole tiles round it (u from 0 to round(2 pi r / tile) at its first
// point's radius, at least 1) rather than once round, so a thick trunk's bark is not stretched, and its lobes.
struct TubeOptions {
    bool wholeTilesRound = false;
    TubeLobes lobes;
};

/*
* The tube round a strand, appended to mesh: at each point a ring of sides + 1 vertices (the seam's twice, so u runs 0 to
* 1 round it), turned along the strand without twisting (each ring's frame the last one's, made square to the strand);
* normals out from the strand, v along it in tiles of tileMetres, the tangent round it (+u) with w +1 (the bitangent runs
* down the strand, +v). Triangles face out. With lobes, each ring's radius is lobedRadius at its point's height, and the
* normals and tangents come from the surface's own differences round and along it.
*/
void appendTube(IndexedMesh& mesh, const HangingStrand& strand, int sides, float tileMetres, const TubeOptions& options = {});

/*
* One leaf card appended to mesh: its stem's edge centred on stem, reaching length metres along towardsTip, width across
* (towardsTip x facing); facing is the way its front faces (unit, square to towardsTip), normal the normal its corners
* shade with; its picture the atlas's rectangle from uvMin (stem's edge) to uvMax. Tangent across (+u) made square to
* the normal, w +1.
*/
void appendLeafCard(IndexedMesh& mesh, const Vec3& stem, const Vec3& towardsTip, const Vec3& facing, const Vec3& normal, float length,
                    float width, const Vec2& uvMin, const Vec2& uvMax);

// The texture rectangle of an atlas's cell (counted along rows from the bottom left), as {lower left, upper right}.
struct AtlasCell {
    Vec2 uvMin;
    Vec2 uvMax;
};
AtlasCell atlasCell(int cell, int columns, int rows);

/*
* Ivy leaves along a vine, appended to mesh: one about every spacing metres, each a card size long (stem to tip) and half
* as wide again, as the atlas's leaves are (LeafSet017: six leaves, two across and three up, each cell 3:2), its stem's
* edge on the vine's surface, pointing out from it at a random turn and drooping; each card shows one of the six leaves.
* Tangent across the leaf (+u), bitangent from stem to tip (+v).
*/
void appendLeafCards(IndexedMesh& mesh, const HangingStrand& vine, float spacing, float size, std::uint32_t seed);

// How much hangs from the islands (F5 Forest, Hangings; start-up: the meshes are built once).
struct ForestHangingSettings {
    // Roots and vines for each 10 m of an island's radius (a 60 m island: 18 roots, 12 vines).
    float rootsPerTenMetres = 3.0f;
    float vinesPerTenMetres = 2.0f;

    // The longest root and vine (metres); each is a random 20-100% of it.
    float longestRoot = 50.0f;
    float longestVine = 80.0f;

    // The metres between ivy leaves along a vine, and a leaf's length.
    float leafSpacing = 0.5f;
    float leafSize = 0.4f;
};

void visit(ForestHangingSettings& settings, SettingsVisitor& visitor);

// One island's roots and vines: their tubes in one mesh (bark), the vines' leaves in another, where each was anchored
// and where its tip hangs.
struct IslandHangings {
    IndexedMesh bark;
    IndexedMesh leaves;
    std::vector<Vec3> anchors;
    std::vector<Vec3> tips;
    int rootCount = 0;
    int vineCount = 0;
};

// Any unit direction square to a unit direction.
Vec3 squareTo(const Vec3& direction);

// A direction made level and of unit length (+x when it has no level part): the way out of a shape at a point.
Vec3 levelOutward(const Vec3& direction);

// The points of an island's surface on its underside (below a fiftieth of its radius under the rim), tip first.
std::vector<std::size_t> islandUnderside(const SurfaceGrid& surface, float radius);

// One of them for a random share (0..1): its square root, mostly near 1, so more often near the rim than the tip.
std::size_t towardsTheRim(const std::vector<std::size_t>& underside, float random);

/*
* An island's roots and vines, in its own frame: as many as the settings give for its radius, each anchored at a point
* of its surface's underside (below a fiftieth of its radius under the rim), chosen more often near the rim than the
* tip, sunk a little into the rock so the tube starts inside it. The same island always grows the same ones.
*/
IslandHangings islandHangings(const IslandPlacement& island, const SurfaceGrid& surface, const ForestHangingSettings& settings);
