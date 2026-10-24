#include "cloth/cloth_constraints.h"

#include "math/angles.h"

#include <algorithm>
#include <cmath>
#include <map>
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

void solveStretch(ClothParticles& particles, const std::vector<StretchConstraint>& constraints, float compliance, float seconds) {
    const float scaledCompliance = compliance / (seconds * seconds);

    for (const StretchConstraint& constraint : constraints) {
        const float firstWeight = particles.inverseMasses[constraint.first];
        const float secondWeight = particles.inverseMasses[constraint.second];
        const float weights = firstWeight + secondWeight;
        if (weights == 0.0f) {
            continue;
        }

        Vec3& first = particles.positions[constraint.first];
        Vec3& second = particles.positions[constraint.second];
        const Vec3 between = first - second;
        const float length = between.length();
        if (length < shortestEdge) {
            continue;
        }

        const Vec3 along = between * (1.0f / length);
        const float multiplier = -(length - constraint.restLength) / (weights + scaledCompliance);
        first = first + along * (firstWeight * multiplier);
        second = second - along * (secondWeight * multiplier);
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
        constraints.push_back(bend);
    }
    return constraints;
}

float hingeAngle(const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd) {
    return hingeState(wing0, wing1, edgeStart, edgeEnd).angle;
}

std::array<Vec3, 4> hingeAngleGradient(const Vec3& wing0, const Vec3& wing1, const Vec3& edgeStart, const Vec3& edgeEnd) {
    return hingeState(wing0, wing1, edgeStart, edgeEnd).gradient;
}

void solveBend(ClothParticles& particles, const std::vector<BendConstraint>& constraints, float bendingStiffness, float seconds) {
    if (bendingStiffness <= 0.0f) {
        return;
    }

    std::vector<Vec3>& positions = particles.positions;
    const float perSecondSquared = 1.0f / (seconds * seconds);
    for (const BendConstraint& bend : constraints) {
        const std::uint32_t points[4]{bend.wing0, bend.wing1, bend.edgeStart, bend.edgeEnd};
        const HingeState state = hingeState(positions[points[0]], positions[points[1]], positions[points[2]], positions[points[3]]);
        const std::array<Vec3, 4>& gradient = state.gradient;

        float moving = 0.0f;
        for (int point = 0; point < 4; ++point) {
            moving += particles.inverseMasses[points[point]] * gradient[point].lengthSquared();
        }
        if (moving == 0.0f) {
            continue;
        }

        const float scaledCompliance = perSecondSquared / (bendingStiffness * bend.stiffnessShare);
        const float error = wrapAngle(state.angle - bend.restAngle);
        const float multiplier = -error / (moving + scaledCompliance);
        for (int point = 0; point < 4; ++point) {
            positions[points[point]] = positions[points[point]] + gradient[point] * (particles.inverseMasses[points[point]] * multiplier);
        }
    }
}
