#pragma once

#include "cloth/cloth_constraints.h"
#include "cloth/cloth_grab.h"
#include "cloth/cloth_particles.h"
#include "core/job_system.h"
#include "math/vec3.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

/*
* A substep's energy (implicit Euler as a minimisation; Martin et al. 2011, Gast et al. 2015): the substep's positions
* are those minimising
*
*   phi(x) = sum m_i |x_i - prediction_i|^2 / (2 h^2) + the fabric's energy (stretch, bending) + the hand's spring,
*
* the first term the cost of leaving where momentum and gravity carry each point. Vertex Block Descent
* (cloth_block_descent.h) and the coarse levels (cloth_hierarchy.h) both lower this one phi, so neither can fight the
* other. XPBD's constraint steps solve the same problem in its dual form (multipliers), which is why a move XPBD did not
* make itself breaks its bookkeeping.
*/

/*
* Each point's constraints, so a point's share of phi can be summed by that point alone (one pass, no colours):
* - edgeStarts, edges: per point, then the end, where its stretch constraints' indices start.
* - hingeStarts, hinges: per point, then the end, its hinges as hinge index times 4 plus which corner it is (wing0,
*   wing1, edgeStart, edgeEnd).
*/
struct ClothPointConstraints {
    std::vector<std::uint32_t> edgeStarts;
    std::vector<std::uint32_t> edges;
    std::vector<std::uint32_t> hingeStarts;
    std::vector<std::uint32_t> hinges;
};

// The lists for these constraints, in the order they are given.
ClothPointConstraints clothPointConstraints(const std::vector<StretchConstraint>& stretch, const std::vector<BendConstraint>& bend,
                                            std::size_t pointCount);

// What a substep's energy is made of besides the points and their prediction: the fabric's constraints and its numbers, and the hand.
struct ClothEnergyTerms {
    const std::vector<StretchConstraint>& stretch;
    const std::vector<BendConstraint>& bend;
    const ClothPointConstraints& pointConstraints;
    float stretchCompliance = 0.0f;
    float bendingStiffness = 0.0f;
    BendingModel bendingModel = BendingModel::Isometric;
    const std::optional<ClothGrab>& grab;
    float grabCompliance = 0.0f;
};

/*
* A compliance of 0 is a perfectly stiff edge or hand, whose energy is infinite away from its rest: the energy treats it
* as this stiff instead (m/N), a thousand times stiffer than cotton.
*/
constexpr float stiffestClothCompliance = 1e-7f;

// The stiffness (1 / compliance) the energy uses for a compliance.
float energyStiffness(float compliance);

// A hinge's C for the model: the curvature vector (isometric), or the angle less its rest in x (hinge).
Vec3 hingeError(const BendConstraint& bend, const std::vector<Vec3>& positions, BendingModel model);

/*
* A symmetric 3 x 3 matrix: the stiffness a point's or a region's step is divided by (its block of phi's second
* derivatives, or an approximation of it).
*/
struct ClothStiffnessBlock {
    float xx = 0.0f, yy = 0.0f, zz = 0.0f, xy = 0.0f, xz = 0.0f, yz = 0.0f;

    void addDiagonal(float value);

    // value times v v^T.
    void addOuter(const Vec3& v, float value);

    // The block's inverse times force, by the adjugate; zero for a block too near singular to trust.
    Vec3 solve(const Vec3& force) const;
};

/*
* phi for a substep of length seconds: the free points' inertia about their prediction, sum m |x - prediction|^2 / (2
* h^2), plus each edge's C^2 / (2 compliance), each hinge's bendingStiffness stiffnessShare |C|^2 / 2, and the hand's
* |x - target|^2 / (2 grabCompliance). In joules; in double, since it sums many small terms.
*/
double clothStepEnergy(const ClothParticles& particles, const std::vector<Vec3>& predicted, const ClothEnergyTerms& terms, float seconds);

/*
* The force on a free point, minus phi's gradient at it: the inertia m (prediction - x) / h^2, minus each of its edges'
* C / compliance along the edge, its hinges' stiffness C dC/dx, the hand's (x - target) / compliance. With a block, its
* 3 x 3 block of phi's second derivatives is added to it too: m / h^2, each edge's stiffness along it and across it
* while stretched (left out while squashed, where that part is negative, so the block stays positive), each hinge's
* (the isometric weight squared, or the angle's gradient's outer product: Gauss-Newton), the hand's. Zero for a pinned
* point. perSecondSquared is 1 / h^2.
*/
Vec3 clothPointForce(std::uint32_t point, const ClothParticles& particles, const std::vector<Vec3>& predicted, const ClothEnergyTerms& terms,
                     float perSecondSquared, ClothStiffnessBlock* block = nullptr);

/*
* The pieces of a point's force and block, each added to what is there (clothPointForce adds them in this order, and
* cloth.glsl mirrors each): its inertia m / h^2 pulling it to its prediction; an edge to another point, by Hooke's law
* along it, stiff along it and, while stretched, across it; an isometric hinge's curvature times the point's weight in
* it; a hinge's angle error along the point's slope of the angle (Gauss-Newton); the hand's spring to its target.
* stiffness is the term's (1 / compliance, or the hinge's bending stiffness times its share). With no block, the force
* alone.
*/
void addInertiaTerm(Vec3& force, ClothStiffnessBlock* block, const Vec3& here, const Vec3& predicted, float inertia);
void addEdgeTerm(Vec3& force, ClothStiffnessBlock* block, const Vec3& here, const Vec3& other, float restLength, float stiffness);
void addIsometricHingeTerm(Vec3& force, ClothStiffnessBlock* block, const Vec3& curvature, float weight, float stiffness);
void addHingeAngleTerm(Vec3& force, ClothStiffnessBlock* block, const Vec3& slope, float error, float stiffness);
void addHandTerm(Vec3& force, ClothStiffnessBlock* block, const Vec3& here, const Vec3& target, float stiffness);

/*
* phi's gradient at every point, zero where held is set (held may be empty: nothing held). A point at a time, so the
* same bits on any number of threads.
*/
void clothEnergyGradient(const ClothParticles& particles, const std::vector<Vec3>& predicted, const ClothEnergyTerms& terms, float seconds,
                         const std::vector<std::uint8_t>& held, std::vector<Vec3>& gradient, JobSystem& jobs);

/*
* Move along direction as far as phi's quadratic model says: by -slope / curvature, slope = gradient . direction and
* curvature = d . H d (each point's inertia m |d|^2 / h^2, each edge's along and while stretched across it, each hinge's
* stiffness |dC . d|^2, the hand's), both summed in one pass in fixed chunks in a fixed order. Nothing when the
* direction does not go downhill. Returns how far (0 for no move).
*/
float clothLineSearch(ClothParticles& particles, const std::vector<Vec3>& gradient, const std::vector<Vec3>& direction,
                      const ClothEnergyTerms& terms, float seconds, JobSystem& jobs);
