#include "cloth/cloth_energy.h"

#include "cloth/cloth_shared.h"
#include "math/angles.h"

#include <array>
#include <cmath>
#include <utility>

namespace {
    // Below this length an edge has no direction (cloth/cloth_shared.h).
    constexpr float shortestEdge = static_cast<float>(CLOTH_SHORTEST_EDGE);

    // Points a job takes at a time (each sums a dozen or so constraints), and constraints a chunk of a sum takes.
    constexpr std::size_t pointsPerJob = 512;
    constexpr std::size_t constraintsPerChunk = 2048;

    // A block whose determinant is below this share of its largest diagonal cubed is taken as singular.
    constexpr float singularBlock = 1e-12f;

    std::array<std::uint32_t, 4> hingePoints(const BendConstraint& bend) {
        return {bend.wing0, bend.wing1, bend.edgeStart, bend.edgeEnd};
    }

    // A point's per-point lists from (point, item) pairs, in the order the pairs are given.
    void buildLists(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& pairs, std::size_t pointCount, std::vector<std::uint32_t>& starts,
                    std::vector<std::uint32_t>& items) {
        starts.assign(pointCount + 1, 0);
        for (const auto& [point, item] : pairs) {
            ++starts[point + 1];
        }
        for (std::size_t point = 0; point < pointCount; ++point) {
            starts[point + 1] += starts[point];
        }
        items.resize(pairs.size());
        std::vector<std::uint32_t> next(starts.begin(), starts.end() - 1);
        for (const auto& [point, item] : pairs) {
            items[next[point]++] = item;
        }
    }

    // An edge's d . H d along the direction (see clothLineSearch).
    double edgeCurvature(const StretchConstraint& edge, const std::vector<Vec3>& positions, const std::vector<Vec3>& direction, double stiffness) {
        const Vec3 between = positions[edge.first] - positions[edge.second];
        const float length = between.length();
        if (length < shortestEdge) {
            return 0.0;
        }
        const Vec3 apart = direction[edge.first] - direction[edge.second];
        const double along = apart.dot(between) / length;
        const double across = std::max(0.0, double(apart.lengthSquared()) - along * along);
        const double stretched = std::max(0.0, 1.0 - double(edge.restLength) / length);
        return stiffness * (along * along + stretched * across);
    }

    // A hinge's d . H d along the direction: exact for the isometric model, Gauss-Newton for the angle.
    double hingeCurvature(const BendConstraint& bend, const std::vector<Vec3>& positions, const std::vector<Vec3>& direction, float bendingStiffness,
                          BendingModel model) {
        const std::array<std::uint32_t, 4> points = hingePoints(bend);
        const double stiffness = double(bendingStiffness) * bend.stiffnessShare;
        if (model == BendingModel::Isometric) {
            return stiffness * isometricCurvature(bend.isometricWeights, direction[points[0]], direction[points[1]], direction[points[2]],
                                                  direction[points[3]])
                                   .lengthSquared();
        }
        const std::array<Vec3, 4> angleGradient = hingeAngleGradient(positions[points[0]], positions[points[1]], positions[points[2]], positions[points[3]]);
        double change = 0.0;
        for (int corner = 0; corner < 4; ++corner) {
            change += angleGradient[corner].dot(direction[points[corner]]);
        }
        return stiffness * change * change;
    }
}

ClothPointConstraints clothPointConstraints(const std::vector<StretchConstraint>& stretch, const std::vector<BendConstraint>& bend,
                                            std::size_t pointCount) {
    ClothPointConstraints lists;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edgePairs;
    for (std::uint32_t index = 0; index < stretch.size(); ++index) {
        edgePairs.emplace_back(stretch[index].first, index);
        edgePairs.emplace_back(stretch[index].second, index);
    }
    buildLists(edgePairs, pointCount, lists.edgeStarts, lists.edges);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> hingePairs;
    for (std::uint32_t index = 0; index < bend.size(); ++index) {
        const std::array<std::uint32_t, 4> points = hingePoints(bend[index]);
        for (std::uint32_t corner = 0; corner < 4; ++corner) {
            hingePairs.emplace_back(points[corner], index * 4 + corner);
        }
    }
    buildLists(hingePairs, pointCount, lists.hingeStarts, lists.hinges);
    return lists;
}

void ClothStiffnessBlock::addDiagonal(float value) {
    xx += value;
    yy += value;
    zz += value;
}

void ClothStiffnessBlock::addOuter(const Vec3& v, float value) {
    xx += value * v.x * v.x;
    yy += value * v.y * v.y;
    zz += value * v.z * v.z;
    xy += value * v.x * v.y;
    xz += value * v.x * v.z;
    yz += value * v.y * v.z;
}

Vec3 ClothStiffnessBlock::solve(const Vec3& force) const {
    const float cofactorXX = yy * zz - yz * yz;
    const float cofactorXY = xz * yz - xy * zz;
    const float cofactorXZ = xy * yz - xz * yy;
    const float determinant = xx * cofactorXX + xy * cofactorXY + xz * cofactorXZ;
    const float size = std::max(xx, std::max(yy, zz));
    if (!(determinant > singularBlock * size * size * size)) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }
    const float cofactorYY = xx * zz - xz * xz;
    const float cofactorYZ = xy * xz - xx * yz;
    const float cofactorZZ = xx * yy - xy * xy;
    const float scale = 1.0f / determinant;
    return Vec3{(cofactorXX * force.x + cofactorXY * force.y + cofactorXZ * force.z) * scale,
                (cofactorXY * force.x + cofactorYY * force.y + cofactorYZ * force.z) * scale,
                (cofactorXZ * force.x + cofactorYZ * force.y + cofactorZZ * force.z) * scale};
}

float energyStiffness(float compliance) {
    return 1.0f / std::max(compliance, stiffestClothCompliance);
}

Vec3 hingeError(const BendConstraint& bend, const std::vector<Vec3>& positions, BendingModel model) {
    const std::array<std::uint32_t, 4> points = hingePoints(bend);
    if (model == BendingModel::Isometric) {
        return isometricCurvature(bend.isometricWeights, positions[points[0]], positions[points[1]], positions[points[2]], positions[points[3]]);
    }
    const float angle = hingeAngle(positions[points[0]], positions[points[1]], positions[points[2]], positions[points[3]]);
    return Vec3{wrapAngle(angle - bend.restAngle), 0.0f, 0.0f};
}

double clothStepEnergy(const ClothParticles& particles, const std::vector<Vec3>& predicted, const ClothEnergyTerms& terms, float seconds) {
    const std::vector<Vec3>& positions = particles.positions;
    const double perSecondSquared = 1.0 / (double(seconds) * seconds);
    double energy = 0.0;
    for (std::size_t point = 0; point < particles.size(); ++point) {
        if (particles.inverseMasses[point] > 0.0f) {
            energy += 0.5 * perSecondSquared * (positions[point] - predicted[point]).lengthSquared() / particles.inverseMasses[point];
        }
    }

    const double stretchStiffness = energyStiffness(terms.stretchCompliance);
    for (const StretchConstraint& edge : terms.stretch) {
        const double stretch = double((positions[edge.first] - positions[edge.second]).length()) - edge.restLength;
        energy += 0.5 * stretchStiffness * stretch * stretch;
    }
    if (terms.bendingStiffness > 0.0f) {
        for (const BendConstraint& bend : terms.bend) {
            energy += 0.5 * double(terms.bendingStiffness) * bend.stiffnessShare * hingeError(bend, positions, terms.bendingModel).lengthSquared();
        }
    }
    if (terms.grab) {
        energy += 0.5 * double(energyStiffness(terms.grabCompliance)) * (positions[terms.grab->particle] - terms.grab->target).lengthSquared();
    }
    return energy;
}

Vec3 clothPointForce(std::uint32_t point, const ClothParticles& particles, const std::vector<Vec3>& predicted, const ClothEnergyTerms& terms,
                     float perSecondSquared, ClothStiffnessBlock* block) {
    const float inverseMass = particles.inverseMasses[point];
    if (inverseMass == 0.0f) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }
    const std::vector<Vec3>& positions = particles.positions;
    const ClothPointConstraints& lists = terms.pointConstraints;
    const Vec3 here = positions[point];
    const float inertia = perSecondSquared / inverseMass;
    Vec3 force = (predicted[point] - here) * inertia;
    if (block) {
        block->addDiagonal(inertia);
    }

    const float stretchStiffness = energyStiffness(terms.stretchCompliance);
    for (std::uint32_t at = lists.edgeStarts[point]; at < lists.edgeStarts[point + 1]; ++at) {
        const StretchConstraint& edge = terms.stretch[lists.edges[at]];
        const Vec3 between = here - positions[edge.first == point ? edge.second : edge.first];
        const float length = between.length();
        if (length < shortestEdge) {
            continue;
        }
        const Vec3 along = between * (1.0f / length);
        force = force - along * (stretchStiffness * (length - edge.restLength));
        if (block) {
            const float across = std::max(0.0f, 1.0f - edge.restLength / length);
            block->addDiagonal(stretchStiffness * across);
            block->addOuter(along, stretchStiffness * (1.0f - across));
        }
    }

    if (terms.bendingStiffness > 0.0f) {
        for (std::uint32_t at = lists.hingeStarts[point]; at < lists.hingeStarts[point + 1]; ++at) {
            const BendConstraint& bend = terms.bend[lists.hinges[at] / 4];
            const std::uint32_t corner = lists.hinges[at] % 4;
            const float stiffness = terms.bendingStiffness * bend.stiffnessShare;
            const Vec3 error = hingeError(bend, positions, terms.bendingModel);
            if (terms.bendingModel == BendingModel::Isometric) {
                const float weight = bend.isometricWeights[corner];
                force = force - error * (stiffness * weight);
                if (block) {
                    block->addDiagonal(stiffness * weight * weight);
                }
            } else {
                const Vec3 slope = hingeAngleGradient(positions[bend.wing0], positions[bend.wing1], positions[bend.edgeStart], positions[bend.edgeEnd])[corner];
                force = force - slope * (stiffness * error.x);
                if (block) {
                    block->addOuter(slope, stiffness);
                }
            }
        }
    }

    if (terms.grab && terms.grab->particle == point) {
        const float stiffness = energyStiffness(terms.grabCompliance);
        force = force - (here - terms.grab->target) * stiffness;
        if (block) {
            block->addDiagonal(stiffness);
        }
    }
    return force;
}

void clothEnergyGradient(const ClothParticles& particles, const std::vector<Vec3>& predicted, const ClothEnergyTerms& terms, float seconds,
                         const std::vector<std::uint8_t>& held, std::vector<Vec3>& gradient, JobSystem& jobs) {
    const float perSecondSquared = 1.0f / (seconds * seconds);
    gradient.resize(particles.size());
    jobs.parallelFor(particles.size(), pointsPerJob, [&](std::size_t point) {
        const bool isHeld = !held.empty() && held[point] != 0;
        gradient[point] = isHeld ? Vec3{0.0f, 0.0f, 0.0f}
                                 : clothPointForce(static_cast<std::uint32_t>(point), particles, predicted, terms, perSecondSquared) * -1.0f;
    });
}

/*
* Slope and curvature in one pass split three ways: chunks of points (the slope and the inertia), of edges and of
* hinges, each term counted once and the chunks added in their fixed order.
*/
float clothLineSearch(ClothParticles& particles, const std::vector<Vec3>& gradient, const std::vector<Vec3>& direction,
                      const ClothEnergyTerms& terms, float seconds, JobSystem& jobs) {
    const std::vector<Vec3>& positions = particles.positions;
    const double perSecondSquared = 1.0 / (double(seconds) * seconds);
    const double stretchStiffness = energyStiffness(terms.stretchCompliance);
    const double grabStiffness = energyStiffness(terms.grabCompliance);

    // Chunks of points, then of edges, then of hinges, in one pass: each term once, in a fixed order.
    const std::size_t pointCount = particles.size();
    const std::size_t pointChunks = (pointCount + pointsPerJob - 1) / pointsPerJob;
    const std::size_t edgeChunks = (terms.stretch.size() + constraintsPerChunk - 1) / constraintsPerChunk;
    const std::size_t hingeChunks = terms.bendingStiffness > 0.0f ? (terms.bend.size() + constraintsPerChunk - 1) / constraintsPerChunk : 0;
    const std::size_t chunks = pointChunks + edgeChunks + hingeChunks;
    std::vector<double> slopes(chunks, 0.0);
    std::vector<double> curvatures(chunks, 0.0);
    jobs.parallelFor(chunks, 1, [&](std::size_t chunk) {
        double slope = 0.0;
        double curvature = 0.0;
        if (chunk < pointChunks) {
            const std::size_t end = std::min(pointCount, (chunk + 1) * pointsPerJob);
            for (std::size_t point = chunk * pointsPerJob; point < end; ++point) {
                slope += double(gradient[point].dot(direction[point]));
                const float inverseMass = particles.inverseMasses[point];
                if (inverseMass > 0.0f) {
                    curvature += direction[point].lengthSquared() * perSecondSquared / inverseMass;
                }
            }
        } else if (chunk < pointChunks + edgeChunks) {
            const std::size_t first = (chunk - pointChunks) * constraintsPerChunk;
            const std::size_t end = std::min(terms.stretch.size(), first + constraintsPerChunk);
            for (std::size_t edge = first; edge < end; ++edge) {
                curvature += edgeCurvature(terms.stretch[edge], positions, direction, stretchStiffness);
            }
        } else {
            const std::size_t first = (chunk - pointChunks - edgeChunks) * constraintsPerChunk;
            const std::size_t end = std::min(terms.bend.size(), first + constraintsPerChunk);
            for (std::size_t hinge = first; hinge < end; ++hinge) {
                curvature += hingeCurvature(terms.bend[hinge], positions, direction, terms.bendingStiffness, terms.bendingModel);
            }
        }
        slopes[chunk] = slope;
        curvatures[chunk] = curvature;
    });
    if (terms.grab) {
        curvatures.push_back(grabStiffness * direction[terms.grab->particle].lengthSquared());
        slopes.push_back(0.0);
    }

    double slope = 0.0;
    double curvature = 0.0;
    for (std::size_t chunk = 0; chunk < slopes.size(); ++chunk) {
        slope += slopes[chunk];
        curvature += curvatures[chunk];
    }
    if (!(slope < 0.0) || !(curvature > 0.0)) {
        return 0.0f;
    }
    const float length = static_cast<float>(-slope / curvature);
    jobs.parallelFor(pointCount, pointsPerJob, [&](std::size_t point) {
        particles.positions[point] = particles.positions[point] + direction[point] * length;
    });
    return length;
}
