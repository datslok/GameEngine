#include "cloth/cloth_block_descent.h"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace {
    // Points a job takes at a time: each is a few dozen constraints' sums.
    constexpr std::size_t pointsPerJob = 256;
}

ClothVertexColours clothVertexColours(const std::vector<StretchConstraint>& stretch, const std::vector<BendConstraint>& bend,
                                      const ClothPointConstraints& pointConstraints) {
    const std::size_t pointCount = pointConstraints.edgeStarts.size() - 1;
    std::vector<int> colourOf(pointCount, -1);
    int colourCount = 0;
    for (std::uint32_t point = 0; point < pointCount; ++point) {
        std::uint64_t used = 0;
        const auto mark = [&](std::uint32_t other) {
            if (other != point && colourOf[other] >= 0) {
                used |= std::uint64_t{1} << colourOf[other];
            }
        };
        for (std::uint32_t at = pointConstraints.edgeStarts[point]; at < pointConstraints.edgeStarts[point + 1]; ++at) {
            mark(stretch[pointConstraints.edges[at]].first);
            mark(stretch[pointConstraints.edges[at]].second);
        }
        for (std::uint32_t at = pointConstraints.hingeStarts[point]; at < pointConstraints.hingeStarts[point + 1]; ++at) {
            const BendConstraint& hinge = bend[pointConstraints.hinges[at] / 4];
            for (const std::uint32_t other : {hinge.wing0, hinge.wing1, hinge.edgeStart, hinge.edgeEnd}) {
                mark(other);
            }
        }
        if (~used == 0) {
            throw std::length_error("A cloth's points need more than 64 colours");
        }
        colourOf[point] = std::countr_one(used);
        colourCount = std::max(colourCount, colourOf[point] + 1);
    }

    ClothVertexColours colours;
    for (int colour = 0; colour < colourCount; ++colour) {
        colours.colours.push_back(static_cast<std::uint32_t>(colours.order.size()));
        for (std::uint32_t point = 0; point < pointCount; ++point) {
            if (colourOf[point] == colour) {
                colours.order.push_back(point);
            }
        }
    }
    colours.colours.push_back(static_cast<std::uint32_t>(colours.order.size()));
    return colours;
}

// Each point's step reads only its own constraints' points, none of which share its colour, and writes only itself.
void solveVertexBlocks(ClothParticles& particles, const std::vector<Vec3>& predicted, const ClothVertexColours& colours, const ClothEnergyTerms& terms,
                       float seconds, JobSystem& jobs) {
    const float perSecondSquared = 1.0f / (seconds * seconds);
    for (std::size_t colour = 0; colour + 1 < colours.colours.size(); ++colour) {
        const std::uint32_t first = colours.colours[colour];
        jobs.parallelFor(colours.colours[colour + 1] - first, pointsPerJob, [&](std::size_t index) {
            const std::uint32_t point = colours.order[first + index];
            ClothStiffnessBlock block;
            const Vec3 force = clothPointForce(point, particles, predicted, terms, perSecondSquared, &block);
            if (particles.inverseMasses[point] > 0.0f) {
                particles.positions[point] = particles.positions[point] + block.solve(force);
            }
        });
    }
}
