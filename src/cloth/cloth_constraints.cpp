#include "cloth/cloth_constraints.h"

#include <algorithm>
#include <utility>

namespace {
    // Below this length an edge has no direction to push along.
    constexpr float shortestEdge = 1e-9f;
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
