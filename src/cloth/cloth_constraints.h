#pragma once

#include "cloth/cloth_particles.h"
#include "scene/mesh.h"

#include <cstdint>
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
