#pragma once

#include "cloth/cloth_particles.h"
#include "cloth/cloth_shared.h"
#include "core/job_system.h"
#include "math/vec2.h"
#include "math/vec3.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

/*
* Tethers (long-range attachments, Kim, Chentanez & Muller 2012): an invisible string from each point to each of its
* nearest anchors (pinned points), as long as the cloth between them at rest plus a little slack,
*
*   C = |x - anchor| - restLength (1 + slack) <= 0,
*
* acting only when taut. A local solver carries a pin's pull about one edge a pass, so a fine sheet hangs stretched
* (the yardstick: 1% at 25 squares a side, 36% at 200); the string reaches the pin straight away, whatever the mesh. It
* never stops the cloth folding, bunching or swinging round its pins, only stretching away from them, and cloth with
* nothing pinned gets nothing from it.
*/
constexpr std::size_t tethersPerPoint = CLOTH_TETHERS_PER_POINT;
constexpr std::uint32_t noTetherAnchor = CLOTH_NO_ANCHOR;

static_assert(noTetherAnchor == std::numeric_limits<std::uint32_t>::max(), "A spare tether slot's anchor is no point a cloth can have");

struct ClothTether {
    std::uint32_t anchor = noTetherAnchor;
    float restLength = 0.0f; // m
};

/*
* A cloth cut from flat pieces (a sewing pattern, cloth_pattern.h), as tethers measure it: each point's piece, and each
* piece's outline, its border's points in order round it, counterclockwise in the rest shape's x-y plane, where the
* pieces lie flat. Empty for a cloth of one piece with no inward corners (a sheet), measured by straight lines.
*/
struct ClothPieces {
    std::vector<std::uint32_t> pieceOf;
    std::vector<std::vector<std::uint32_t>> outlines;
};

/*
* How far a cloth's points are from each of some anchors along the cloth at rest: how long a tether must be, since a
* tether shorter than the fabric between its ends would pull the fabric tight.
*
* On a flat piece that is the shortest path inside its outline: a string pulled taut inside it, straight but for bends
* round the outline's inward corners (an armhole's or a neckline's curve), never across them. The shortest path between
* two points inside a polygon only ever bends at its reflex corners (those turning inwards), so it is found on a
* visibility graph: those corners are its nodes, linked where the straight line between them stays inside the outline;
* each anchor's distance to every corner comes from Dijkstra's algorithm over it, once; a point's distance to an
* anchor is then the straight line if it sees the anchor, or the best of a corner it sees plus that corner's distance.
* A point and an anchor on different pieces are infinitely far apart: no tether crosses a seam, so a dress's front and
* back each hang from their own anchors. Without pieces, the straight line between the points at rest.
*/
class ClothRestPaths {
public:
    ClothRestPaths(const std::vector<Vec3>& restPositions, const ClothPieces& pieces, const std::vector<std::uint32_t>& anchors);

    // The point's distance to each anchor along the cloth at rest, in the anchors' order, into distances.
    void distances(std::uint32_t point, std::vector<float>& distances) const;

private:
    struct Piece {
        std::vector<Vec2> outline;
        std::vector<Vec2> corners;                         // the outline's reflex corners
        std::vector<std::uint32_t> anchors;                // which of the anchors lie on the piece
        std::vector<std::vector<float>> cornerDistances;   // [anchor on the piece][corner]: along the piece
    };

    bool sees(const Piece& piece, const Vec2& from, const Vec2& to) const;

    std::vector<Vec3> restPositions;
    ClothPieces pieces;
    std::vector<std::uint32_t> anchors;
    std::vector<Piece> flatPieces;
};

/*
* Every point's tethers, tethersPerPoint slots each (point i's from i * tethersPerPoint): its nearest anchors, nearest
* first, a tie in the anchors' order, at their distance along the cloth at rest (ClothRestPaths, on its own piece
* alone); the spare slots, and every slot of a pinned point, hold noTetherAnchor. Empty when nothing is pinned. The
* anchors are the points of inverse mass 0. The points are shared among the jobs' threads, the same on any number.
*/
std::vector<ClothTether> clothTethers(const std::vector<Vec3>& restPositions, const std::vector<float>& inverseMasses,
                                      const ClothPieces& pieces = {}, JobSystem& jobs = serialJobs());

/*
* One tether on its point (solveTethers' step, mirrored by cloth.glsl's clothTether): a point further from the anchor
* than restLength times stretchAllowed (1 + the slack) goes back onto that sphere along the line from the anchor; a
* point inside it is left exactly where it is.
*/
inline void solveTether(Vec3& position, const Vec3& anchor, float restLength, float stretchAllowed) {
    const Vec3 away = position - anchor;
    const float length = away.length();
    const float longest = restLength * stretchAllowed;
    if (length > longest) {
        position = anchor + away * (longest / length);
    }
}

/*
* One pass over the tethers, each taut one solved as XPBD with no compliance: the anchor is pinned (inverse mass 0), so
* the multiplier step comes down to putting the point back on its sphere, delta x = -C n with n the unit vector from the
* anchor. A point's tethers are solved nearest first and touch only that point (the anchors never move here), so the
* points are shared among the jobs' threads with the same result as one thread's, to the last bit.
*/
void solveTethers(ClothParticles& particles, const std::vector<ClothTether>& tethers, float slack, JobSystem& jobs);
