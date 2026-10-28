#pragma once

#include "cloth/cloth_particles.h"
#include "core/job_system.h"
#include "scene/mesh.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

/*
* A stretch constraint (XPBD, Macklin, Muller & Chentanez 2016): an edge between two points that wants its rest length,
* C = |x_first - x_second| - restLength.
*/
struct StretchConstraint {
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    float restLength = 0.0f;
};

/*
* Every edge of the mesh's triangles once (first < second), sorted, at its length in the mesh: on a grid, the threads
* along its rows and columns and one diagonal a square, which also resists shearing.
*/
std::vector<StretchConstraint> stretchConstraints(const Mesh& mesh);

/*
* One pass over the edges in order (Gauss-Seidel: each sees the corrections before it), for a step of the given length.
* XPBD's multiplier step with the multiplier starting at 0, since each substep makes one pass:
*
*   delta lambda = -C / (w_first + w_second + compliance / seconds^2)
*
* and each end moves along the edge by its inverse mass times delta lambda, so a pinned end (w = 0) never moves and the
* centre of mass stays put. compliance is 1 / stiffness, in m/N: 0 holds the length exactly, and a positive value makes
* the edge a spring of that stiffness whatever the step. Edges with both ends pinned, or squashed to a point, are skipped.
*/
void solveStretch(ClothParticles& particles, const std::vector<StretchConstraint>& constraints, float compliance, float seconds);

/*
* A bending constraint on a hinge: two triangles sharing the edge edgeStart-edgeEnd, their far corners wing0 and wing1,
* C = angle - restAngle, the angle between the triangles (0 flat). stiffnessShare is 3 l^2 / A from the rest shape (l the
* edge's length, A the two triangles' area; discrete shells, Grinspun et al. 2003), so a hinge's compliance is
* 1 / (B stiffnessShare) for a fabric of bending rigidity B (N m), and the same B drapes alike at any mesh resolution.
*
* The same hinge also carries isometric bending's weights (isometricBendWeights), for the cheaper model, which shares
* the points, the stiffness share and so the colouring.
*/
struct BendConstraint {
    std::uint32_t wing0 = 0;
    std::uint32_t wing1 = 0;
    std::uint32_t edgeStart = 0;
    std::uint32_t edgeEnd = 0;
    float restAngle = 0.0f;
    float stiffnessShare = 0.0f;
    std::array<float, 4> isometricWeights{}; // wing0, wing1, edgeStart, edgeEnd
};

/*
* How a cloth bends:
* - Isometric (Bergou, Wardetzky, Harmon, Zorin & Grinspun 2006, "A Quadratic Bending Model for Inextensible Surfaces"):
*   cloth barely stretches, so its bending energy is exactly quadratic in the positions, through a fixed weighted sum of
*   the hinge's points. About three times cheaper than the angle; always rests flat, as cloth cut from flat panels does.
* - Hinge: the true angle between the triangles (Bridson's gradient), resting at the mesh's own angles, for creases
*   pressed into a curved rest shape (a pleat).
*/
enum class BendingModel {
    Isometric,
    Hinge,
};

// Every edge of the mesh shared by exactly two triangles, sorted by its ends, at its angle in the mesh.
std::vector<BendConstraint> bendConstraints(const Mesh& mesh);

/*
* The hinge on points (wing0, wing1, edgeStart, edgeEnd) resting where rest puts them: its rest angle, stiffness share
* and isometric weights. None for a squashed hinge (a triangle or the edge with no size), which could bend nothing.
*/
std::optional<BendConstraint> restHinge(const std::array<std::uint32_t, 4>& points, const std::array<Vec3, 4>& rest);

/*
* Isometric bending's weights K for a hinge's points (wing0, wing1, edgeStart, edgeEnd): each wing 1 / its distance from
* the edge's line, each end of the edge minus the wings' weights shared by where along the edge their feet lie (the
* nearer end taking more). They sum to zero and send any flat hinge to zero, so moving or turning flat cloth bends
* nothing. Bergou et al.'s cotangent weights times -1 / l, scaled so the curvature below is about the angle: a hinge's
* energy 1/2 B stiffnessShare |C|^2 then matches the angle model's for small folds. From each triangle's own shape, so
* the same for a hinge made folded. All zero for a squashed hinge.
*/
std::array<float, 4> isometricBendWeights(const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd);

/*
* The hinge's curvature C = sum of K_i x_i, a vector: zero flat, and 2 sin(angle / 2) long for a hinge folded by an
* angle without stretching (the angle itself while small).
*/
Vec3 isometricCurvature(const std::array<float, 4>& weights, const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd);

/*
* The hinge's angle, -pi..pi: 0 flat, its sign telling which way it is folded. From atan2 of the sine and cosine
* between the triangles' normals, not acos of the cosine, whose slope is infinite exactly at flat. 0 for a hinge
* squashed so a triangle or the edge has no size.
*/
float hingeAngle(const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd);

/*
* How the angle changes as each point moves (wing0, wing1, edgeStart, edgeEnd), in closed form (Bridson, Marino &
* Fedkiw 2003): the wings along their triangles' normals, inversely to their distance from the edge, the edge's ends by
* how far along it the wings lie. They sum to zero. All zero for a squashed hinge.
*/
std::array<Vec3, 4> hingeAngleGradient(const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd);

/*
* One pass over the hinges in order, XPBD's multiplier step (starting at 0): delta lambda = -C / (sum of w |grad|^2 +
* alpha / seconds^2), alpha = 1 / (bendingStiffness stiffnessShare), each point moved by its w times its gradient times
* delta lambda. No bending at all for a stiffness of 0.
* - Hinge: C is the angle less the rest angle, wrapped to -pi..pi, its gradient Bridson's.
* - Isometric: C is the curvature vector, whose gradient for point i is just K_i (times the identity), so the step is
*   one vector, delta lambda = -C / (sum of w_i K_i^2 + alpha / seconds^2), and point i moves by w_i K_i delta lambda:
*   no normals, angles or square roots. The weights sum to zero, so the centre of mass stays put.
* A solve standing for several substeps of seconds (bendingSubsteps) divides alpha / seconds^2 by them: the correction
* becomes velocity over one substep, so this gives the impulse that many substeps' bending would.
*/
void solveBend(ClothParticles& particles, const std::vector<BendConstraint>& constraints, float bendingStiffness, float seconds, BendingModel model,
               int substepsCovered = 1);

/*
* Graph colouring: reorder the constraints into colours, groups in which no two share a point, each constraint taking
* the lowest colour none of its points has yet, the hardest to fit first (DSatur), the constraints of a colour kept in
* their order. The fewer colours the fewer GPU dispatches: a mesh's edges take as many as meet at its busiest point,
* the fewest possible. Returns where
* each colour starts, then the end. Within a colour the constraints are independent, so they can be solved on many
* threads at once with the same result to the last bit; solving the colours in turn is still Gauss-Seidel. Throws
* std::length_error past 64 colours (a point in more than about 30 constraints).
*/
std::vector<std::uint32_t> colourStretch(std::vector<StretchConstraint>& constraints, std::size_t pointCount);
std::vector<std::uint32_t> colourBend(std::vector<BendConstraint>& constraints, std::size_t pointCount);

// solveStretch and solveBend over coloured constraints, each colour's constraints shared among the jobs' threads (a
// colour with no more than a job's worth runs on the caller). The same result as the one-thread solves, to the last bit.
void solveStretchColours(ClothParticles& particles, const std::vector<StretchConstraint>& constraints, const std::vector<std::uint32_t>& colours,
                         float compliance, float seconds, JobSystem& jobs);
void solveBendColours(ClothParticles& particles, const std::vector<BendConstraint>& constraints, const std::vector<std::uint32_t>& colours,
                      float bendingStiffness, float seconds, BendingModel model, JobSystem& jobs, int substepsCovered = 1);
