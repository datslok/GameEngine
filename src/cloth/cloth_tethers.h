#pragma once

#include "cloth/cloth_particles.h"
#include "cloth/cloth_shared.h"
#include "core/job_system.h"
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
* How far apart two points are along the cloth at rest. For now the straight line between them in the rest shape:
* exact for a flat piece with no inward corners, as every cloth so far is. A dress needs the shortest path over the
* sewn pattern (across seams, round armholes: exact geodesics, Xin & Wang 2009), which only this function changes for.
*/
float clothRestDistance(const std::vector<Vec3>& restPositions, std::uint32_t from, std::uint32_t to);

/*
* Every point's tethers, tethersPerPoint slots each (point i's from i * tethersPerPoint): its nearest anchors, nearest
* first, a tie in the anchors' order, at their distance along the cloth at rest; the spare slots, and every slot of a
* pinned point, hold noTetherAnchor. Empty when nothing is pinned. The anchors are the points of inverse mass 0.
*/
std::vector<ClothTether> clothTethers(const std::vector<Vec3>& restPositions, const std::vector<float>& inverseMasses);

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
