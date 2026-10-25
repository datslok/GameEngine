#pragma once

#include "cloth/cloth_constraints.h"
#include "cloth/cloth_energy.h"
#include "cloth/cloth_particles.h"
#include "core/job_system.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/*
* Vertex Block Descent (Chen, Liu, Yang & Yuksel 2024): the substep's energy phi (cloth_energy.h) lowered one point at
* a time, each point taking the exact Newton step for its own position with every other point held still: its force
* (minus phi's gradient at it) divided by its 3 x 3 block of phi's second derivatives,
*
*   block = m / h^2 + each edge's stiffness along it (and across it while stretched) + its hinges' + the hand's.
*
* Points sharing no edge or hinge are independent, so the points are coloured and each colour is solved on every core
* at once (Gauss-Seidel by colour, as XPBD's constraints are). Unlike XPBD it works on positions alone, with no
* multipliers to keep, so a coarse level's move (cloth_hierarchy.h) is simply where the next sweep starts from.
*/

// The points sorted by colour, in index order within a colour (order), and where each colour starts, then the end.
struct ClothVertexColours {
    std::vector<std::uint32_t> order;
    std::vector<std::uint32_t> colours;
};

/*
* Each point coloured the lowest colour none of the points sharing an edge or a hinge with it has, in index order.
* Throws std::length_error past 64 colours.
*/
ClothVertexColours clothVertexColours(const std::vector<StretchConstraint>& stretch, const std::vector<BendConstraint>& bend,
                                      const ClothPointConstraints& pointConstraints);

/*
* One sweep: every free point's Newton step, colour by colour, from phi's terms and the substep's prediction. Pinned
* points stay. The same bits on any number of threads.
*/
void solveVertexBlocks(ClothParticles& particles, const std::vector<Vec3>& predicted, const ClothVertexColours& colours, const ClothEnergyTerms& terms,
                       float seconds, JobSystem& jobs);
