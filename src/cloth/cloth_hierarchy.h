#pragma once

#include "cloth/cloth_constraints.h"
#include "cloth/cloth_grab.h"
#include "cloth/cloth_particles.h"
#include "core/job_system.h"
#include "math/vec3.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

/*
* The low-frequency layer (Muller 2008, "Hierarchical Position Based Dynamics"; docs/CLOTHGPU.md, Part 2). A pass over
* the edges carries a correction about one edge, so a pin's pull crosses a fine sheet slowly and the sheet stretches.
* Coarser copies of the cloth carry it across in a few edges: each level keeps a subset of the points below it, joined
* by one-sided limits that only stop over-stretch, never folding or bending, and each substep solves them coarsest
* first, passing only their corrections down, so the fine level keeps its folds and wrinkles.
*/

// The most coarse levels a cloth is given (a 200 x 200 sheet makes 9); the settings choose how many a tick uses.
constexpr int maxClothLevels = 12;

/*
* How many coarse neighbours (parents) every fine point keeps: the paper's k. The paper uses 2; on the yardstick 3 left
* a tenth of the stretch and made deep hierarchies stable (docs/BUILDLOG.md), each level about 2.2 times smaller than
* the one below instead of 3, its children following three or more parents instead of two.
*/
constexpr int clothParentsWanted = 3;

/*
* No level is made with fewer points than this. Measured on the yardstick: every hierarchy that shook a settled sheet
* apart ended in a level of about 30 points or fewer, spanning the sheet in a few edges, and none with 50 or more did.
*/
constexpr std::size_t smallestClothLevel = 50;

// A point on the level above a child's, and its share of the child's motion.
struct ClothParent {
    std::uint32_t point = 0;
    float weight = 0.0f;
};

/*
* One coarse level: its points (the cloth's indices, sorted), the limits between them (StretchConstraint's layout, each
* at the straight distance between its ends in the rest shape, solved one-sided), coloured, and its children: the points
* of the level below not on this one, which follow their parents here.
*/
struct ClothLevel {
    std::vector<std::uint32_t> points;
    std::vector<StretchConstraint> limits;
    std::vector<std::uint32_t> limitColours;
    std::vector<std::uint32_t> children;
};

/*
* A cloth's levels, levels[0] being level 1, the first coarser than the cloth itself (level 0). A point's level is the
* coarsest that holds it; a point not on level 1 or above has parents on the level above its own, listed per point.
*/
struct ClothHierarchy {
    std::vector<ClothLevel> levels;
    std::vector<int> pointLevels;            // per point
    std::vector<std::uint32_t> parentStarts; // per point, then the end: where its parents start in parents
    std::vector<ClothParent> parents;
};

/*
* Which points stay on the coarser level, by the paper's greedy sweep: all start coarse; visited in order, a point goes
* fine if it keeps at least clothParentsWanted coarse neighbours and every fine neighbour keeps more than that, so each
* still has enough after it goes. keptCoarse points (pins: they anchor the coarse levels) never go fine. The same
* neighbours always give the same choice.
*/
std::vector<bool> chooseCoarsePoints(const std::vector<std::vector<std::uint32_t>>& neighbours, const std::vector<bool>& keptCoarse);

/*
* A child's weights for its parents, by their rest distances d: 1 / (d / the farthest d + epsilon), summing to 1, so the
* nearer parent leads and a parent at the child itself takes almost all (epsilon keeps that finite). They pass on a
* shift of the parents exactly, a stretch or turn only roughly; weights that pass those on too (least squares, kept near
* these) were tried and shook the sheet apart, being negative where a child lies outside its parents.
*/
std::vector<float> parentWeights(const Vec3& child, const std::vector<Vec3>& parents);

/*
* The levels of a cloth with these edges, in its rest shape, until a level would stop shrinking or hold fewer than
* smallestClothLevel points, or there are maxClothLevels; a small cloth has none. Each level
* is made by collapsing every fine point into one of its coarse neighbours (the one nearest their mean, which keeps
* edges even), its edges moved to that neighbour, so a triangle mesh stays one. Pinned points (inverse mass 0) are kept
* on every level.
*/
ClothHierarchy buildClothHierarchy(const std::vector<Vec3>& restPositions, const std::vector<StretchConstraint>& edges,
                                   const std::vector<float>& inverseMasses);

// The point standing for a point on a level: itself if it is on that level, else its heaviest parent's stand-in.
std::uint32_t pointOnLevel(const ClothHierarchy& hierarchy, std::uint32_t point, int level);

/*
* The hierarchy's part of a substep, before the cloth's own constraints, using levels 1 to `levels` (all it has if
* fewer). Every point on level 1 has its position saved in starts (sized as the cloth's points). Then, from the coarsest
* level used down to level 1, the level's points not on the level above follow their parents' whole motion since the
* save, so every coarser level's pull reaches them, not just the last one's (the paper re-saves on each level, which
* drops it: docs/CLOTHGPU.md, Part 2); then the level's limits are solved, and the hand pulls the point standing for the
* held one as far as the held one still has to go. Last, the cloth's points not on level 1 follow their parents. Pinned
* points never move.
*/
void solveClothHierarchy(ClothParticles& particles, const ClothHierarchy& hierarchy, int levels, float limitCompliance,
                         const std::optional<ClothGrab>& grab, float grabCompliance, float seconds, std::vector<Vec3>& starts, JobSystem& jobs);
