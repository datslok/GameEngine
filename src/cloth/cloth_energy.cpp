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

    // A block whose determinant is below this share of its largest diagonal cubed is taken as singular (cloth/cloth_shared.h).
    constexpr float singularBlock = static_cast<float>(CLOTH_SINGULAR_BLOCK);

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

    // A hinge's d . H d along the direction: exact for the isometric model, Gauss-Newton for the angle.
    double hingeCurvature(const BendConstraint& bend, const std::vector<Vec3>& positions, const std::vector<Vec3>& direction, float bendingStiffness,
                          BendingModel model) {
        const std::array<std::uint32_t, 4> points = hingePoints(bend);
        const double stiffness = double(bendingStiffness) * bend.stiffnessShare;
        if (model == BendingModel::Isometric) {
            return isometricHingeCurvature(bend.isometricWeights, direction[points[0]], direction[points[1]], direction[points[2]], direction[points[3]],
                                           stiffness);
        }
        const std::array<Vec3, 4> angleGradient = hingeAngleGradient(positions[points[0]], positions[points[1]], positions[points[2]], positions[points[3]]);
        return hingeAngleCurvature(angleGradient, direction[points[0]], direction[points[1]], direction[points[2]], direction[points[3]], stiffness);
    }
}

double inertiaCurvature(const Vec3& direction, double perSecondSquared, float inverseMass) {
    return direction.lengthSquared() * perSecondSquared / inverseMass;
}

double edgeCurvature(const Vec3& between, const Vec3& apart, float restLength, double stiffness) {
    const float length = between.length();
    if (length < shortestEdge) {
        return 0.0;
    }
    const double along = apart.dot(between) / length;
    const double across = std::max(0.0, double(apart.lengthSquared()) - along * along);
    const double stretched = std::max(0.0, 1.0 - double(restLength) / length);
    return stiffness * (along * along + stretched * across);
}

double isometricHingeCurvature(const std::array<float, 4>& weights, const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd,
                               double stiffness) {
    return stiffness * isometricCurvature(weights, wing0, wing1, edgeStart, edgeEnd).lengthSquared();
}

double hingeAngleCurvature(const std::array<Vec3, 4>& slopes, const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd,
                           double stiffness) {
    double change = 0.0;
    change += slopes[0].dot(wing0);
    change += slopes[1].dot(wing1);
    change += slopes[2].dot(edgeStart);
    change += slopes[3].dot(edgeEnd);
    return stiffness * change * change;
}

double handCurvature(const Vec3& direction, double stiffness) {
    return stiffness * direction.lengthSquared();
}

float lineSearchStep(double slope, double curvature) {
    if (!(slope < 0.0) || !(curvature > 0.0)) {
        return 0.0f;
    }
    return static_cast<float>(-slope / curvature);
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

void addInertiaTerm(Vec3& force, ClothStiffnessBlock* block, const Vec3& here, const Vec3& predicted, float inertia) {
    force = force + (predicted - here) * inertia;
    if (block) {
        block->addDiagonal(inertia);
    }
}

void addEdgeTerm(Vec3& force, ClothStiffnessBlock* block, const Vec3& here, const Vec3& other, float restLength, float stiffness) {
    const Vec3 between = here - other;
    const float length = between.length();
    if (length < shortestEdge) {
        return;
    }
    const Vec3 along = between * (1.0f / length);
    force = force - along * (stiffness * (length - restLength));
    if (block) {
        const float across = std::max(0.0f, 1.0f - restLength / length);
        block->addDiagonal(stiffness * across);
        block->addOuter(along, stiffness * (1.0f - across));
    }
}

void addIsometricHingeTerm(Vec3& force, ClothStiffnessBlock* block, const Vec3& curvature, float weight, float stiffness) {
    force = force - curvature * (stiffness * weight);
    if (block) {
        block->addDiagonal(stiffness * weight * weight);
    }
}

void addHingeAngleTerm(Vec3& force, ClothStiffnessBlock* block, const Vec3& slope, float error, float stiffness) {
    force = force - slope * (stiffness * error);
    if (block) {
        block->addOuter(slope, stiffness);
    }
}

void addHandTerm(Vec3& force, ClothStiffnessBlock* block, const Vec3& here, const Vec3& target, float stiffness) {
    force = force - (here - target) * stiffness;
    if (block) {
        block->addDiagonal(stiffness);
    }
}

// The terms in the order cloth_pass.glsl's clothPointForceAt adds them.
Vec3 clothPointForce(std::uint32_t point, const ClothParticles& particles, const std::vector<Vec3>& predicted, const ClothEnergyTerms& terms,
                     float perSecondSquared, ClothStiffnessBlock* block) {
    const float inverseMass = particles.inverseMasses[point];
    if (inverseMass == 0.0f) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }
    const std::vector<Vec3>& positions = particles.positions;
    const ClothPointConstraints& lists = terms.pointConstraints;
    const Vec3 here = positions[point];
    Vec3 force{0.0f, 0.0f, 0.0f};
    addInertiaTerm(force, block, here, predicted[point], perSecondSquared / inverseMass);

    const float stretchStiffness = energyStiffness(terms.stretchCompliance);
    for (std::uint32_t at = lists.edgeStarts[point]; at < lists.edgeStarts[point + 1]; ++at) {
        const StretchConstraint& edge = terms.stretch[lists.edges[at]];
        addEdgeTerm(force, block, here, positions[edge.first == point ? edge.second : edge.first], edge.restLength, stretchStiffness);
    }

    if (terms.bendingStiffness > 0.0f) {
        for (std::uint32_t at = lists.hingeStarts[point]; at < lists.hingeStarts[point + 1]; ++at) {
            const BendConstraint& bend = terms.bend[lists.hinges[at] / 4];
            const std::uint32_t corner = lists.hinges[at] % 4;
            const float stiffness = terms.bendingStiffness * bend.stiffnessShare;
            const Vec3 error = hingeError(bend, positions, terms.bendingModel);
            if (terms.bendingModel == BendingModel::Isometric) {
                addIsometricHingeTerm(force, block, error, bend.isometricWeights[corner], stiffness);
            } else {
                const Vec3 slope = hingeAngleGradient(positions[bend.wing0], positions[bend.wing1], positions[bend.edgeStart], positions[bend.edgeEnd])[corner];
                addHingeAngleTerm(force, block, slope, error.x, stiffness);
            }
        }
    }

    if (terms.grab && terms.grab->particle == point) {
        addHandTerm(force, block, here, terms.grab->target, energyStiffness(terms.grabCompliance));
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
                    curvature += inertiaCurvature(direction[point], perSecondSquared, inverseMass);
                }
            }
        } else if (chunk < pointChunks + edgeChunks) {
            const std::size_t first = (chunk - pointChunks) * constraintsPerChunk;
            const std::size_t end = std::min(terms.stretch.size(), first + constraintsPerChunk);
            for (std::size_t index = first; index < end; ++index) {
                const StretchConstraint& edge = terms.stretch[index];
                curvature += edgeCurvature(positions[edge.first] - positions[edge.second], direction[edge.first] - direction[edge.second],
                                           edge.restLength, stretchStiffness);
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
        curvatures.push_back(handCurvature(direction[terms.grab->particle], grabStiffness));
        slopes.push_back(0.0);
    }

    double slope = 0.0;
    double curvature = 0.0;
    for (std::size_t chunk = 0; chunk < slopes.size(); ++chunk) {
        slope += slopes[chunk];
        curvature += curvatures[chunk];
    }
    const float length = lineSearchStep(slope, curvature);
    if (length == 0.0f) {
        return 0.0f;
    }
    jobs.parallelFor(pointCount, pointsPerJob, [&](std::size_t point) {
        particles.positions[point] = particles.positions[point] + direction[point] * length;
    });
    return length;
}
