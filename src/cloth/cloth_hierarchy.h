#pragma once

#include "cloth/cloth_constraints.h"
#include "cloth/cloth_energy.h"
#include "cloth/cloth_particles.h"
#include "core/job_system.h"
#include "math/vec3.h"
#include "physics/shapes.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/*
* The low-frequency layer (docs/CLOTHGPU.md, Part 2). A pass over the edges carries a correction about one edge, so a
* pin's pull crosses a fine sheet slowly and the sheet stretches. Coarser levels carry it across in a few steps.
*
* The levels are Muller 2008's (Hierarchical Position Based Dynamics): each keeps a subset of the points below it, and
* every other point follows its parents above. But what each level does is Galerkin multigrid's (Lu et al. 2025, "Fast
* Galerkin Multigrid"), not Muller's one-sided limits: a substep is an implicit Euler step, the positions minimising
*
*   phi(x) = sum m_i |x_i - prediction_i|^2 / (2 h^2) + the fabric's energy (stretch, bending, the hand's spring),
*
* and each coarse level takes a step that lowers that same phi, moving whole regions with their real masses. Muller's
* limits aimed at zero stretch, a different rest than the fabric's, and moved regions as if they weighed a point: they
* raised phi every substep (measured), and sagging sheets shook forever.
*/

// The most coarse levels a cloth is given (a 200 x 200 sheet makes 9); the settings choose how many a tick uses.
constexpr int maxClothLevels = 12;

/*
* How many coarse neighbours (parents) every fine point keeps: the paper's k. The paper uses 2; 3 gives each child more
* parents to follow, the levels about 2.2 times smaller each instead of 3 (docs/BUILDLOG.md).
*/
constexpr int clothParentsWanted = 3;

// No level is made with fewer points than this (docs/BUILDLOG.md).
constexpr std::size_t smallestClothLevel = 50;

// A point on another level and its weight: a child's parent, or a parent's child.
struct ClothLink {
    std::uint32_t point = 0;
    float weight = 0.0f;
};

/*
* One coarse level:
* - points: the cloth's indices on it, sorted, a subset of the level below's.
* - neighbourStarts, neighbours: per point (in points' order), then the end, where its neighbours on this level start:
*   the coarse mesh left by collapsing the finer points, a triangle mesh still.
* - masses: per point, sum over the free points it moves of weight^2 times their mass, the diagonal of P^T M P: what the
*   region it carries weighs.
* - children: the points of the level below not on this one, which follow their parents here.
* - spreadStarts, spread: per cloth point, then the end, its row of P: each level point that moves it and the share
*   (the product of the parents' weights down to it; a level point's own row is itself at 1).
* - gatherStarts, gather: per level point, then the end, its column of P: the cloth points it moves and their shares.
* With P straight from the cloth's points, gathering and spreading take one pass each instead of one per level between.
*/
struct ClothLevel {
    std::vector<std::uint32_t> points;
    std::vector<std::uint32_t> neighbourStarts;
    std::vector<std::uint32_t> neighbours;
    std::vector<float> masses;
    std::vector<std::uint32_t> children;
    std::vector<std::uint32_t> spreadStarts;
    std::vector<ClothLink> spread;
    std::vector<std::uint32_t> gatherStarts;
    std::vector<ClothLink> gather;
};

/*
* A cloth's levels, levels[0] being level 1, the first coarser than the cloth itself (level 0). A point's level is the
* coarsest that holds it; a point not on level 1 or above has parents on the level above its own, listed per point.
* The weights chain: a point on level L moves the points below it by the products of the weights down to them, the
* prolongation P (prolongFromLevel), whose transpose gathers values up (restrictToLevel).
*/
struct ClothHierarchy {
    std::vector<ClothLevel> levels;
    std::vector<int> pointLevels;            // per point
    std::vector<std::uint32_t> parentStarts; // per point, then the end: where its parents start in parents
    std::vector<ClothLink> parents;
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
* nearer parent leads and a parent at the child itself takes almost all (epsilon keeps that finite).
*/
std::vector<float> parentWeights(const Vec3& child, const std::vector<Vec3>& parents);

/*
* The levels of a cloth with these edges, in its rest shape, with these inverse masses, until a level would stop
* shrinking or hold fewer than smallestClothLevel points, or there are maxClothLevels; a small cloth has none. Each level
* is made by collapsing every fine point into one of its parents (the one nearest their mean, which keeps edges even),
* its edges moved to that parent, so a triangle mesh stays one. Pinned points (inverse mass 0) are kept on every level.
*/
ClothHierarchy buildClothHierarchy(const std::vector<Vec3>& restPositions, const std::vector<StretchConstraint>& edges,
                                   const std::vector<float>& inverseMasses);

/*
* values, one per point, gathered up to a level in place: on return each point on that level holds the sum over the
* points it moves of their values times its weight for them (P^T values); other points' values are spent.
*/
void restrictToLevel(const ClothHierarchy& hierarchy, std::vector<Vec3>& values, int level, JobSystem& jobs = serialJobs());

/*
* values given on a level's points spread down in place: on return every point below the level holds its weighted share
* of them (P values), each child the sum of its parents' values times its weights.
*/
void prolongFromLevel(const ClothHierarchy& hierarchy, std::vector<Vec3>& values, int level, JobSystem& jobs = serialJobs());

/*
* A coarse edge's share of its point's block: the fabric's stiffness along the edge's direction now (between, from the
* point to its neighbour), as the outer product between between^T times stiffness / |between|^2; nothing for an edge
* too short to have a direction. cloth.glsl mirrors it.
*/
void addCoarseEdgeTerm(ClothStiffnessBlock& block, const Vec3& between, float stiffness);

// Work arrays for the hierarchy's steps, one value per point, kept between substeps so they are not made again.
struct ClothHierarchyScratch {
    std::vector<Vec3> predicted;
    std::vector<Vec3> gradient;
    std::vector<Vec3> direction;
    std::vector<Vec3> coarseSteps;
    std::vector<std::uint8_t> held;
};

/*
* The hierarchy's part of a substep, after the prediction (kept in scratch.predicted by the caller) and before the
* cloth's own constraints, using levels 1 to `levels` (all it has if fewer), coarsest first. Held points never move: the
* pinned, and those touching a body (within thickness of one of the bodies), which the contacts hold; they are left out
* of the levels altogether, gathering nothing and following nothing (truncated, as monotone multigrid does).
*
* On each level, from phi's gradient at the points now:
* 1. Gather it up the level (P^T gradient).
* 2. Each coarse point's step: minus that, divided by its region's 3 x 3 stiffness: its mass over h^2 (masses, exact)
*    plus its coarse edges as springs of the fabric's compliance along their directions now (an approximation of the
*    fabric's share, which only shapes the direction).
* 3. Spread the steps down to every point (P), a direction d.
* 4. Go along d as far as phi's own curvature says: t = -(gradient . d) / (d . H d), H phi's second derivatives (each
*    edge's along its direction, and across it while stretched), so the step lowers phi and its length needs no tuning.
* Where the cloth is already at phi's minimum the gradient is zero and nothing moves: the levels never fight the fine
* solve. Sums are made in fixed chunks in a fixed order, so any number of threads gives the same bits.
*/
void solveClothHierarchy(ClothParticles& particles, const ClothHierarchy& hierarchy, int levels, const ClothEnergyTerms& terms,
                         const std::vector<Shape>& bodies, float thickness, float seconds, ClothHierarchyScratch& scratch, JobSystem& jobs);

