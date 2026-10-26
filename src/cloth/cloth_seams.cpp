#include "cloth/cloth_seams.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <utility>

void solveStitches(ClothParticles& particles, const std::vector<ClothStitch>& stitches, float closingStep) {
    for (const ClothStitch& stitch : stitches) {
        const float firstWeight = particles.inverseMasses[stitch.first];
        const float secondWeight = particles.inverseMasses[stitch.second];
        const float weights = firstWeight + secondWeight;
        if (weights <= 0.0f) {
            continue;
        }

        const float widest = (particles.previousPositions[stitch.second] - particles.previousPositions[stitch.first]).length() - closingStep;
        Vec3& first = particles.positions[stitch.first];
        Vec3& second = particles.positions[stitch.second];
        const Vec3 gap = second - first;
        if (widest <= 0.0f) {
            const Vec3 joined = first + gap * (firstWeight / weights);
            first = joined;
            second = joined;
            continue;
        }

        const float distance = gap.length();
        if (distance <= widest) {
            continue;
        }
        const Vec3 move = gap * ((distance - widest) / (distance * weights));
        first = first + move * firstWeight;
        second = second - move * secondWeight;
    }
}

namespace {
    using Side = std::pair<std::uint32_t, std::uint32_t>;

    Side sideOf(std::uint32_t a, std::uint32_t b) {
        return {std::min(a, b), std::max(a, b)};
    }

    // The far corner of the triangle on each side, by the side's ends: a border side has one, an inside side two.
    std::map<Side, std::vector<std::uint32_t>> farCorners(const std::vector<std::array<std::uint32_t, 3>>& triangles) {
        std::map<Side, std::vector<std::uint32_t>> corners;
        for (const std::array<std::uint32_t, 3>& triangle : triangles) {
            for (int corner = 0; corner < 3; ++corner) {
                corners[sideOf(triangle[(corner + 1) % 3], triangle[(corner + 2) % 3])].push_back(triangle[corner]);
            }
        }
        return corners;
    }

    std::optional<std::uint32_t> borderCorner(const std::map<Side, std::vector<std::uint32_t>>& corners, std::uint32_t a, std::uint32_t b) {
        const auto found = corners.find(sideOf(a, b));
        if (found == corners.end() || found->second.size() != 1) {
            return std::nullopt;
        }
        return found->second.front();
    }

    // A corner's place beside a segment: its share of the way along it and its distance from the segment's line.
    struct Beside {
        float along = 0.0f;
        float across = 0.0f;
    };

    Beside besideSegment(const Vec3& corner, const Vec3& start, const Vec3& end) {
        const Vec3 segment = end - start;
        const float along = (corner - start).dot(segment) / segment.lengthSquared();
        return Beside{along, (corner - start - segment * along).length()};
    }
}

/*
* The second piece's corner is put back beside the first's segment at its own share along and its own distance across,
* on the side opposite the first's corner, in the first triangle's plane: the second triangle hinged open flat.
*/
std::vector<BendConstraint> seamHinges(const std::vector<Vec3>& restPositions, const std::vector<std::array<std::uint32_t, 3>>& triangles,
                                       const std::vector<ClothStitch>& seam) {
    const std::map<Side, std::vector<std::uint32_t>> corners = farCorners(triangles);
    std::vector<BendConstraint> hinges;
    for (std::size_t index = 0; index + 1 < seam.size(); ++index) {
        const ClothStitch& start = seam[index];
        const ClothStitch& end = seam[index + 1];
        const std::optional<std::uint32_t> firstWing = borderCorner(corners, start.first, end.first);
        const std::optional<std::uint32_t> secondWing = borderCorner(corners, start.second, end.second);
        if (!firstWing || !secondWing) {
            continue;
        }

        const Vec3& edgeStart = restPositions[start.first];
        const Vec3& edgeEnd = restPositions[end.first];
        const Vec3 segment = edgeEnd - edgeStart;
        const Vec3& wing = restPositions[*firstWing];
        const Vec3 wingOffset = wing - edgeStart - segment * ((wing - edgeStart).dot(segment) / segment.lengthSquared());
        if (wingOffset.lengthSquared() == 0.0f) {
            continue;
        }

        const Beside other = besideSegment(restPositions[*secondWing], restPositions[start.second], restPositions[end.second]);
        const Vec3 unfolded = edgeStart + segment * other.along - wingOffset.normalized() * other.across;
        if (const std::optional<BendConstraint> hinge = restHinge({*firstWing, *secondWing, start.first, end.first}, {wing, unfolded, edgeStart, edgeEnd})) {
            hinges.push_back(*hinge);
        }
    }
    return hinges;
}
