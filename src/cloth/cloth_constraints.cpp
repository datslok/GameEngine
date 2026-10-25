#include "cloth/cloth_constraints.h"

#include "math/angles.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

namespace {
    // Below this length an edge has no direction to push along.
    constexpr float shortestEdge = 1e-9f;

    // Below this a hinge's edge, or a triangle's doubled area, squared, is taken as squashed.
    constexpr float squashedHinge = 1e-12f;

    Vec3 at(const Mesh& mesh, std::uint32_t index) {
        const Vec4& vertex = mesh.vertices.at(index);
        return Vec3{vertex.x, vertex.y, vertex.z};
    }

    // The hinge's two triangles' normals, each scaled by twice its area, as Bridson et al. define them, and the edge.
    struct HingeFrame {
        Vec3 normal0;
        Vec3 normal1;
        Vec3 edge;
    };

    HingeFrame hingeFrame(const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd) {
        return HingeFrame{(wing0 - edgeStart).cross(wing0 - edgeEnd), (wing1 - edgeEnd).cross(wing1 - edgeStart), edgeEnd - edgeStart};
    }

    bool squashed(const HingeFrame& frame) {
        return frame.edge.lengthSquared() < squashedHinge || frame.normal0.lengthSquared() < squashedHinge ||
               frame.normal1.lengthSquared() < squashedHinge;
    }

    // A hinge's angle and its gradient, from one frame (the solve needs both, and building the frame is most of the work).
    struct HingeState {
        float angle = 0.0f;
        std::array<Vec3, 4> gradient{};
    };

    /*
    * atan2 does not change when its two arguments are scaled by the same positive number, so the normals need not be
    * normalised: |normal0| |normal1| scales both the sine and the cosine. Measured from normal1 towards normal0 about the
    * edge: the sign Bridson et al.'s gradient is the gradient of (checked against finite differences).
    */
    HingeState hingeState(const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd) {
        const HingeFrame frame = hingeFrame(wing0, wing1, edgeStart, edgeEnd);
        if (squashed(frame)) {
            return {};
        }

        const float edgeLength = frame.edge.length();
        const Vec3 along = frame.edge * (1.0f / edgeLength);
        const Vec3 scaled0 = frame.normal0 * (1.0f / frame.normal0.lengthSquared());
        const Vec3 scaled1 = frame.normal1 * (1.0f / frame.normal1.lengthSquared());

        HingeState state;
        state.angle = std::atan2(frame.normal1.cross(frame.normal0).dot(along), frame.normal0.dot(frame.normal1));
        state.gradient = {scaled0 * edgeLength,
                          scaled1 * edgeLength,
                          scaled0 * (wing0 - edgeEnd).dot(along) + scaled1 * (wing1 - edgeEnd).dot(along),
                          (scaled0 * (wing0 - edgeStart).dot(along) + scaled1 * (wing1 - edgeStart).dot(along)) * -1.0f};
        return state;
    }
}

std::vector<StretchConstraint> stretchConstraints(const Mesh& mesh) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    edges.reserve(mesh.triangles.size() * 3);
    for (const Triangle& triangle : mesh.triangles) {
        const std::uint32_t corners[3]{static_cast<std::uint32_t>(triangle.first), static_cast<std::uint32_t>(triangle.second),
                                       static_cast<std::uint32_t>(triangle.third)};
        for (int side = 0; side < 3; ++side) {
            const std::uint32_t a = corners[side];
            const std::uint32_t b = corners[(side + 1) % 3];
            edges.emplace_back(std::min(a, b), std::max(a, b));
        }
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

    std::vector<StretchConstraint> constraints;
    constraints.reserve(edges.size());
    for (const auto& [first, second] : edges) {
        const Vec4& a = mesh.vertices.at(first);
        const Vec4& b = mesh.vertices.at(second);
        constraints.push_back(StretchConstraint{first, second, (Vec3{a.x, a.y, a.z} - Vec3{b.x, b.y, b.z}).length()});
    }
    return constraints;
}

namespace {
    // One edge's XPBD step (see solveStretch); scaledCompliance is the compliance over the step's length squared.
    inline void solveStretchConstraint(ClothParticles& particles, const StretchConstraint& constraint, float scaledCompliance) {
        const float firstWeight = particles.inverseMasses[constraint.first];
        const float secondWeight = particles.inverseMasses[constraint.second];
        const float weights = firstWeight + secondWeight;
        if (weights == 0.0f) {
            return;
        }

        Vec3& first = particles.positions[constraint.first];
        Vec3& second = particles.positions[constraint.second];
        const Vec3 between = first - second;
        const float length = between.length();
        if (length < shortestEdge) {
            return;
        }

        const Vec3 along = between * (1.0f / length);
        const float multiplier = -(length - constraint.restLength) / (weights + scaledCompliance);
        first = first + along * (firstWeight * multiplier);
        second = second - along * (secondWeight * multiplier);
    }
}

void solveStretch(ClothParticles& particles, const std::vector<StretchConstraint>& constraints, float compliance, float seconds) {
    const float scaledCompliance = compliance / (seconds * seconds);
    for (const StretchConstraint& constraint : constraints) {
        solveStretchConstraint(particles, constraint, scaledCompliance);
    }
}

/*
* Each edge remembers the far corners of the triangles it belongs to; only edges with exactly two are hinges (the
* border's have one).
*/
std::vector<BendConstraint> bendConstraints(const Mesh& mesh) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>> wingsOfEdge;
    for (const Triangle& triangle : mesh.triangles) {
        const std::uint32_t corners[3]{static_cast<std::uint32_t>(triangle.first), static_cast<std::uint32_t>(triangle.second),
                                       static_cast<std::uint32_t>(triangle.third)};
        for (int side = 0; side < 3; ++side) {
            const std::uint32_t a = corners[side];
            const std::uint32_t b = corners[(side + 1) % 3];
            wingsOfEdge[{std::min(a, b), std::max(a, b)}].push_back(corners[(side + 2) % 3]);
        }
    }

    std::vector<BendConstraint> constraints;
    for (const auto& [edge, wings] : wingsOfEdge) {
        if (wings.size() != 2) {
            continue;
        }

        BendConstraint bend{wings[0], wings[1], edge.first, edge.second, 0.0f, 0.0f};
        const Vec3 wing0 = at(mesh, bend.wing0);
        const Vec3 wing1 = at(mesh, bend.wing1);
        const Vec3 edgeStart = at(mesh, bend.edgeStart);
        const Vec3 edgeEnd = at(mesh, bend.edgeEnd);
        const HingeFrame frame = hingeFrame(wing0, wing1, edgeStart, edgeEnd);
        const float area = 0.5f * (frame.normal0.length() + frame.normal1.length());
        if (squashed(frame)) {
            continue;
        }
        bend.restAngle = hingeAngle(wing0, wing1, edgeStart, edgeEnd);
        bend.stiffnessShare = 3.0f * frame.edge.lengthSquared() / area;
        bend.isometricWeights = isometricBendWeights(wing0, wing1, edgeStart, edgeEnd);
        constraints.push_back(bend);
    }
    return constraints;
}

/*
* A triangle's doubled area is the edge's length times the wing's height, so the height comes from the frame's normals.
* Each wing's foot lies a share t along the edge; giving the ends -(K (1 - t)) and -(K t) puts the weights' sum at the
* foot, so for a flat hinge the wings' pulls across the edge (K h each way) cancel and so do those along it.
*/
std::array<float, 4> isometricBendWeights(const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd) {
    const HingeFrame frame = hingeFrame(wing0, wing1, edgeStart, edgeEnd);
    if (squashed(frame)) {
        return {};
    }

    const float lengthSquared = frame.edge.lengthSquared();
    const float edgeLength = std::sqrt(lengthSquared);
    const float weight0 = edgeLength / frame.normal0.length();
    const float weight1 = edgeLength / frame.normal1.length();
    const float along0 = (wing0 - edgeStart).dot(frame.edge) / lengthSquared;
    const float along1 = (wing1 - edgeStart).dot(frame.edge) / lengthSquared;
    return {weight0, weight1, -(weight0 * (1.0f - along0) + weight1 * (1.0f - along1)), -(weight0 * along0 + weight1 * along1)};
}

Vec3 isometricCurvature(const std::array<float, 4>& weights, const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd) {
    return wing0 * weights[0] + wing1 * weights[1] + edgeStart * weights[2] + edgeEnd * weights[3];
}

float hingeAngle(const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd) {
    return hingeState(wing0, wing1, edgeStart, edgeEnd).angle;
}

std::array<Vec3, 4> hingeAngleGradient(const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd) {
    return hingeState(wing0, wing1, edgeStart, edgeEnd).gradient;
}

namespace {
    // One hinge's XPBD step (see solveBend).
    inline void solveBendConstraint(ClothParticles& particles, const BendConstraint& bend, float bendingStiffness, float perSecondSquared) {
        std::vector<Vec3>& positions = particles.positions;
        const std::uint32_t points[4]{bend.wing0, bend.wing1, bend.edgeStart, bend.edgeEnd};
        const HingeState state = hingeState(positions[points[0]], positions[points[1]], positions[points[2]], positions[points[3]]);
        const std::array<Vec3, 4>& gradient = state.gradient;

        float moving = 0.0f;
        for (int point = 0; point < 4; ++point) {
            moving += particles.inverseMasses[points[point]] * gradient[point].lengthSquared();
        }
        if (moving == 0.0f) {
            return;
        }

        const float scaledCompliance = perSecondSquared / (bendingStiffness * bend.stiffnessShare);
        const float error = wrapAngle(state.angle - bend.restAngle);
        const float multiplier = -error / (moving + scaledCompliance);
        for (int point = 0; point < 4; ++point) {
            positions[points[point]] = positions[points[point]] + gradient[point] * (particles.inverseMasses[points[point]] * multiplier);
        }
    }

    // One hinge's isometric XPBD step (see solveBend).
    inline void solveIsometricBendConstraint(ClothParticles& particles, const BendConstraint& bend, float bendingStiffness, float perSecondSquared) {
        std::vector<Vec3>& positions = particles.positions;
        const std::uint32_t points[4]{bend.wing0, bend.wing1, bend.edgeStart, bend.edgeEnd};
        const std::array<float, 4>& weights = bend.isometricWeights;

        float moving = 0.0f;
        Vec3 curvature{0.0f, 0.0f, 0.0f};
        for (int point = 0; point < 4; ++point) {
            moving += particles.inverseMasses[points[point]] * weights[point] * weights[point];
            curvature = curvature + positions[points[point]] * weights[point];
        }
        if (moving == 0.0f) {
            return;
        }

        const float scaledCompliance = perSecondSquared / (bendingStiffness * bend.stiffnessShare);
        const Vec3 multiplier = curvature * (-1.0f / (moving + scaledCompliance));
        for (int point = 0; point < 4; ++point) {
            positions[points[point]] = positions[points[point]] + multiplier * (particles.inverseMasses[points[point]] * weights[point]);
        }
    }

    /*
    * Constraints a job takes at a time, picked by measuring the big sheet (100 x 100) over a sweep: stretch about 20
    * microseconds a job, hinge bending about 12. Smaller jobs cost more to hand out than they save; larger ones leave
    * threads idle. A sheet the size of the cloth lab's others has colours of a job or two, so they run on the caller.
    * Isometric hinges are so cheap that their jobs must be big: each colour touches nearly every point, and handing
    * small pieces to other cores moves the points' cache lines between them every colour, costing more than the
    * arithmetic (256 a job: 23 ms a tick on 16 threads against 7 on one; 2048: 6.1).
    */
    constexpr std::size_t stretchPerJob = 2048;
    constexpr std::size_t hingeBendPerJob = 256;
    constexpr std::size_t isometricBendPerJob = 2048;

    // Calls solve with the model's one-hinge step and job size, chosen once for the whole pass rather than per hinge.
    template <typename Solve>
    void withBendStep(BendingModel model, Solve solve) {
        if (model == BendingModel::Isometric) {
            solve([](ClothParticles& particles, const BendConstraint& bend, float stiffness, float perSecondSquared) {
                solveIsometricBendConstraint(particles, bend, stiffness, perSecondSquared);
            }, isometricBendPerJob);
        } else {
            solve([](ClothParticles& particles, const BendConstraint& bend, float stiffness, float perSecondSquared) {
                solveBendConstraint(particles, bend, stiffness, perSecondSquared);
            }, hingeBendPerJob);
        }
    }

    // The lowest colour none of the points has, marked on them; the colour numbers as given by pointsOf.
    template <typename Constraint, typename PointsOf>
    std::vector<std::uint32_t> colourConstraints(std::vector<Constraint>& constraints, std::size_t pointCount, PointsOf pointsOf) {
        std::vector<std::uint64_t> usedByPoint(pointCount, 0);
        std::vector<int> colourOf(constraints.size());
        int colourCount = 0;

        for (std::size_t index = 0; index < constraints.size(); ++index) {
            const auto points = pointsOf(constraints[index]);
            std::uint64_t used = 0;
            for (std::uint32_t point : points) {
                used |= usedByPoint.at(point);
            }
            if (~used == 0) {
                throw std::length_error("A cloth's constraints need more than 64 colours");
            }
            const int colour = std::countr_one(used);
            for (std::uint32_t point : points) {
                usedByPoint[point] |= std::uint64_t{1} << colour;
            }
            colourOf[index] = colour;
            colourCount = std::max(colourCount, colour + 1);
        }

        std::vector<Constraint> sorted;
        sorted.reserve(constraints.size());
        std::vector<std::uint32_t> starts;
        for (int colour = 0; colour < colourCount; ++colour) {
            starts.push_back(static_cast<std::uint32_t>(sorted.size()));
            for (std::size_t index = 0; index < constraints.size(); ++index) {
                if (colourOf[index] == colour) {
                    sorted.push_back(constraints[index]);
                }
            }
        }
        starts.push_back(static_cast<std::uint32_t>(sorted.size()));
        constraints = std::move(sorted);
        return starts;
    }
}

void solveBend(ClothParticles& particles, const std::vector<BendConstraint>& constraints, float bendingStiffness, float seconds, BendingModel model) {
    if (bendingStiffness <= 0.0f) {
        return;
    }

    const float perSecondSquared = 1.0f / (seconds * seconds);
    withBendStep(model, [&](auto step, std::size_t) {
        for (const BendConstraint& bend : constraints) {
            step(particles, bend, bendingStiffness, perSecondSquared);
        }
    });
}

std::vector<std::uint32_t> colourStretch(std::vector<StretchConstraint>& constraints, std::size_t pointCount) {
    return colourConstraints(constraints, pointCount, [](const StretchConstraint& edge) {
        return std::array<std::uint32_t, 2>{edge.first, edge.second};
    });
}

std::vector<std::uint32_t> colourBend(std::vector<BendConstraint>& constraints, std::size_t pointCount) {
    return colourConstraints(constraints, pointCount, [](const BendConstraint& hinge) {
        return std::array<std::uint32_t, 4>{hinge.wing0, hinge.wing1, hinge.edgeStart, hinge.edgeEnd};
    });
}

void solveStretchColours(ClothParticles& particles, const std::vector<StretchConstraint>& constraints, const std::vector<std::uint32_t>& colours,
                         float compliance, float seconds, JobSystem& jobs) {
    const float scaledCompliance = compliance / (seconds * seconds);
    for (std::size_t colour = 0; colour + 1 < colours.size(); ++colour) {
        const std::uint32_t first = colours[colour];
        jobs.parallelFor(colours[colour + 1] - first, stretchPerJob, [&](std::size_t index) {
            solveStretchConstraint(particles, constraints[first + index], scaledCompliance);
        });
    }
}

void solveBendColours(ClothParticles& particles, const std::vector<BendConstraint>& constraints, const std::vector<std::uint32_t>& colours,
                      float bendingStiffness, float seconds, BendingModel model, JobSystem& jobs) {
    if (bendingStiffness <= 0.0f) {
        return;
    }

    const float perSecondSquared = 1.0f / (seconds * seconds);
    withBendStep(model, [&](auto step, std::size_t perJob) {
        for (std::size_t colour = 0; colour + 1 < colours.size(); ++colour) {
            const std::uint32_t first = colours[colour];
            jobs.parallelFor(colours[colour + 1] - first, perJob, [&](std::size_t index) {
                step(particles, constraints[first + index], bendingStiffness, perSecondSquared);
            });
        }
    });
}
