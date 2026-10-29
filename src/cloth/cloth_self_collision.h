#pragma once

#include "cloth/cloth_particles.h"
#include "cloth/cloth_seams.h"
#include "cloth/cloth_shared.h"
#include "core/job_system.h"
#include "math/vec3.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

struct ClothSettings;

/*
* Self-collision: folds and layers of one cloth never pass through each other (docs/CLOTHGPU.md, Part 3 and the plan's
* step 5).
*
* Two parts. Contacts keep layers a thickness apart (ClothSettings::thickness, as from bodies): each pair of features
* closer than that, a point and a triangle or two edges, is pushed straight apart along the line between their nearest
* points (Offset Geometric Contact's offset geometry), as an XPBD constraint with no give, shared by inverse mass and
* barycentric weight, with friction as against bodies.
*
* The guarantee comes from a cut (Divide and Truncate, Chen, Hsu, Ayman & Macklin 2026, Planar-DAT): a plane between
* each pair found, and every point's motion since the pairs were found cut short before it reaches any of its planes.
* Each side's room is shared in proportion to how fast it approaches, so a layer falling onto one at rest gets nearly all
* of it, and motion along a plane or away from it is never cut, so sliding and separating are never slowed. Pairs are
* found every few substeps within a reach; a point's motion is also capped at half gamma's share of the reach, since
* anything further than the reach was not looked for. As long as the cloth starts with nothing through anything, nothing
* passes through anything, with no continuous collision detection and no untangling.
*/

// gamma_r: a point moving towards a plane stops this share of the way there.
inline constexpr float clothCutRelaxation = static_cast<float>(CLOTH_CUT_RELAXATION);

/*
* Which features of a cloth never meet. Each point's representative is the first point of its stitch (stitched points
* count as one) or itself; a feature never meets one sharing a representative (a point its own triangles, an edge those
* it shares a corner with). Features one edge apart (each representative's neighbours, sorted) are contacts like any
* other, but are left out when they are already closer than the thickness in the cloth's rest shape: at 1 cm a
* triangle's height (8.7 mm) is over the 5 mm the layers keep, so a flat sheet is left alone, but on a mesh finer than
* about 6 mm the triangles round a point would push it out of its own sheet. Leaving out every neighbour instead let a
* coarse strip fold a square back over the next and through it. Also every edge once, for edge pairs and the crossings
* count, and the rest shape the skip is measured in.
*/
struct ClothSelfTopology {
    std::vector<std::uint32_t> joined;
    std::vector<std::uint32_t> neighbourStarts; // by representative: where its neighbours start, one past the points
    std::vector<std::uint32_t> neighbours;
    std::vector<std::array<std::uint32_t, 2>> edges;
    std::vector<Vec3> restPositions;
    float meanRestEdge = 0.0f;         // m: the hash grid's cells are this and two reaches wide
    std::vector<std::uint32_t> pieceOf; // each point's piece (ClothPieces), or none for a cloth of one piece
    std::vector<std::uint32_t> partners; // each point's stitch partner, or clothNoPartner
};

inline constexpr std::uint32_t clothNoPartner = 4294967295u;

ClothSelfTopology clothSelfTopology(const std::vector<Vec3>& restPositions, const std::vector<std::array<std::uint32_t, 3>>& triangles,
                                    const std::vector<ClothStitch>& stitches, const std::vector<std::uint32_t>& pieceOf = {});

/*
* Whether two nearby features (a point and a triangle's corners, or two edges' ends) are excused from meeting: they are
* neighbours (some point of one is one with or one edge from some point of the other) and either lie on different
* pieces, so across a seam, or were already closer than the thickness at rest (restGap). Across a seam the flat
* pattern's rest shape says nothing of how close they will be once sewn, and a stitch closing brings its point right
* next to its partner's piece: as pairs they held the cloth lab dress's last four stitches 1-2 mm open.
*/
bool clothPairExcused(const ClothSelfTopology& topology, std::span<const std::uint32_t> first, std::span<const std::uint32_t> second, float restGap,
                      float thickness);

// The pair search's hash cells (on the CPU and the GPU): an edge at rest and two reaches wide, so a triangle's box grown
// by the reach covers about two cells a side; at least a tenth of a millimetre.
inline float clothSelfCellSize(float meanRestEdge, float reach) {
    return meanRestEdge + 2.0f * reach > 1e-4f ? meanRestEdge + 2.0f * reach : 1e-4f;
}

// Whether two points are one (the same point, or stitched together) or one edge apart.
bool nearInCloth(const ClothSelfTopology& topology, std::uint32_t first, std::uint32_t second);

enum class ClothPairKind : std::uint8_t {
    PointTriangle = CLOTH_PAIR_POINT_TRIANGLE,
    EdgeEdge = CLOTH_PAIR_EDGE_EDGE,
};

/*
* Two features found close, as they were then: the first side (a point, or an edge's two points) and the second (a
* triangle's three, or an edge's two). normal points from the second side's nearest point to the first's, gap is the
* distance between them, and heights are each point's height above the second side's nearest point along the normal:
* the first side's at least gap, the second's at most 0, since both are convex.
*/
struct ClothSelfPair {
    ClothPairKind kind = ClothPairKind::PointTriangle;
    std::array<std::uint32_t, 4> points{};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float gap = 0.0f;
    std::array<float, 4> heights{};

    // Its dividing plane's height (dividingHeight), placed at the first cut after it was found and kept until the
    // pairs are found again; below zero until then.
    float plane = -1.0f;
};

// How many of a pair's points are on its first side: 1 for a point, 2 for an edge.
int firstSidePoints(ClothPairKind kind);

// A point and a triangle, or two edges, as they are at positions.
ClothSelfPair pointTrianglePair(const std::vector<Vec3>& positions, std::uint32_t point, const std::array<std::uint32_t, 3>& triangle);
ClothSelfPair edgeEdgePair(const std::vector<Vec3>& positions, const std::array<std::uint32_t, 2>& first, const std::array<std::uint32_t, 2>& second);

/*
* Every point and triangle, and every two edges, closer than reach that may meet (ClothSelfTopology: sharing no point,
* and not neighbours already within thickness at rest), in a fixed order (by point, then triangle; by edge, then edge),
* so the same cloth always gives the same pairs, on any number of the jobs' threads.
*/
std::vector<ClothSelfPair> findClothPairs(const std::vector<Vec3>& positions, const std::vector<std::array<std::uint32_t, 3>>& triangles,
                                          const ClothSelfTopology& topology, float reach, float thickness, JobSystem& jobs = serialJobs());

/*
* Planar-DAT's plane for a pair (Eq. 9-11), as its height above the second side's nearest point along the normal, given
* each of its points' motion since the pair was found. Each side's room is the gap shared in proportion to how fast that
* side approaches (its fastest point along the normal): delta_1 = max(-motion . normal), delta_2 = max(motion . normal),
* each at least 0; the first side keeps gap delta_1 / (delta_1 + delta_2), or half the gap when neither approaches.
* (The paper's printed lambda, delta_t / (delta_t + delta_v), would give a point falling onto a still triangle no room
* at all; its text says each side's room is in proportion to its own approach, which this follows.)
*/
float dividingHeight(const ClothSelfPair& pair, const std::array<Vec3, 4>& motions);

/*
* The share of a motion kept (Eq. 16), for a point distance from its plane moving approach towards it: all of it when
* it does not approach, or would only reach the plane at 1 / gamma of its motion or later; otherwise gamma of the way
* to the plane; none when it approaches from on or past the plane (only rounding puts it there).
*/
float cutShare(float distance, float approach);

/*
* The cut, after everything else in a substep: each free point's motion this substep (from substepStart) cut to the
* least share any of its pairs allows, and its motion since the pairs were found (from start) kept within half
* gamma's share of reach. Each pair's plane is placed at its first cut, from the motion since the pairs were found
* (dividingHeight), and kept (ClothSelfPair::plane). Pinned points (inverse mass 0) are never cut. A stitch's two
* points (partners, as for solveSelfContacts) both take the smaller of their shares, so a shut stitch stays shut.
*/
void cutClothMotion(std::vector<Vec3>& positions, const std::vector<Vec3>& start, const std::vector<Vec3>& substepStart,
                    const std::vector<float>& inverseMasses, std::vector<ClothSelfPair>& pairs, float reach,
                    std::span<const std::uint32_t> partners = {});

/*
* One pair's contact (mirrored by cloth.glsl's clothSelfContactMoves): when its nearest points, as the points are, are
* closer than thickness, how each of its four points would move to push them apart to it (straight out along the line
* between them, or along the pair's normal where they meet), shared by inverse mass and barycentric weight, with
* friction on the slide between them since the substep began (previous), as against bodies. False, moving nothing,
* when they are far enough apart or nothing can move. coefficients are each point's weight along the line (+ on the
* first side, - on the second): zero for a point the pair does not move.
*/
bool selfContactMoves(const ClothSelfPair& pair, const std::array<Vec3, 4>& points, const std::array<Vec3, 4>& previous,
                      const std::array<float, 4>& inverseMasses, float thickness, float staticFriction, float kineticFriction,
                      std::array<Vec3, 4>& moves, std::array<float, 4>& coefficients);

// The largest move one pair may give a point (m), and how moves are summed: whole units of CLOTH_SELF_MOVE_SCALE a
// metre, rounded half to even, clamped to the largest; and back, a sum of count of them made one average move.
inline constexpr float clothSelfLargestMove = static_cast<float>(CLOTH_SELF_LARGEST_MOVE);
std::int32_t selfMoveUnits(float metres);
float selfMoveMetres(std::int64_t units, std::uint32_t count);

/*
* The contacts, all at once (Jacobi), as the GPU solves them: each pair's moves worked out from the points as they are
* (selfContactMoves), summed per point in whole units (selfMoveUnits), then each point moved by their average over the
* pairs that move it. Integer sums do not depend on the order they are added in, so the pairs' order makes no
* difference to the last bit (the GPU finds them in no fixed order). A point many pairs press is moved by their
* average rather than their sum, which converges a little slower than one at a time but never overshoots.
*
* A point marked in held (one byte a point, or none: solveContacts' touching) rests on a body this substep and is
* treated as immovable, as the body holds it up: the free layer on it takes the whole push. Pushing a supported layer
* into its body instead made the body push it back out the next substep, over and over: a sheet draped over a ball
* never came to rest, and a strip on the floor lost the gap between its layers.
*
* A stitch's two points (partners: each point's partner, or clothNoPartner; none for a cloth not sewn) move as one:
* their pushes pooled and averaged, neither moved when either is held or pinned. Moved apart, the stitch shut before
* them stood a hair open at the substep's end.
*/
void solveSelfContacts(ClothParticles& particles, const std::vector<ClothSelfPair>& pairs, const ClothSettings& settings,
                       std::span<const std::uint8_t> held = {}, std::span<const std::uint32_t> partners = {});

/*
* For tests: how many edges pass through a triangle sharing no point with them (stitched points as one). Slow, every
* edge against every triangle near it; nothing in a step uses it.
*/
std::size_t countClothCrossings(const std::vector<Vec3>& positions, const std::vector<std::array<std::uint32_t, 3>>& triangles,
                                const ClothSelfTopology& topology);

// The crossings themselves, as (an index into topology.edges, an index into triangles), edge by edge.
std::vector<std::array<std::uint32_t, 2>> clothCrossings(const std::vector<Vec3>& positions, const std::vector<std::array<std::uint32_t, 3>>& triangles,
                                                         const ClothSelfTopology& topology);
