#include "scene/sway_bones.h"
#include "math/quat.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

std::uint32_t packSwayBinding(const SwayBinding& binding) {
    if (binding.bone < 0 || static_cast<std::size_t>(binding.bone) >= swayMostBones) {
        throw std::invalid_argument("A bound vertex's bone must be within a skeleton's 65535 bones");
    }

    const auto weight = static_cast<std::uint32_t>(std::lround(std::clamp(binding.weight, 0.0f, 1.0f) * 65535.0f));
    return static_cast<std::uint32_t>(binding.bone) | (weight << 16u);
}

SwayBinding unpackSwayBinding(std::uint32_t packed) {
    return SwayBinding{.bone = static_cast<int>(packed & 0xFFFFu), .weight = static_cast<float>(packed >> 16u) / 65535.0f};
}

Vec3 swayPlantPoint(const Vec3& world, const Vec3& foot, float scale, float turnRadians) {
    return Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, -turnRadians).rotate(world - foot) * (1.0f / scale);
}

namespace {
    // Each point's distance along the line from its first.
    std::vector<float> arcsOf(const std::vector<Vec3>& points) {
        std::vector<float> arcs(points.size(), 0.0f);

        for (std::size_t k = 1; k < points.size(); ++k) {
            arcs[k] = arcs[k - 1] + (points[k] - points[k - 1]).length();
        }

        return arcs;
    }

    // The point arc along the line and its radius there, between the two points either side.
    std::pair<Vec3, float> pointAlong(const std::vector<Vec3>& points, const std::vector<float>& radii, const std::vector<float>& arcs, float arc) {
        std::size_t segment = 0;

        while (segment + 2 < points.size() && arcs[segment + 1] < arc) {
            ++segment;
        }

        const float span = arcs[segment + 1] - arcs[segment];
        const float share = span > 0.0f ? std::clamp((arc - arcs[segment]) / span, 0.0f, 1.0f) : 0.0f;
        return {points[segment] + (points[segment + 1] - points[segment]) * share, radii[segment] + (radii[segment + 1] - radii[segment]) * share};
    }
}

SwayChain appendSwayChain(std::vector<SwayBone>& bones, const std::vector<Vec3>& points, const std::vector<float>& radii, int parent, int level,
                          float boneMetres) {
    const std::vector<float> arcs = arcsOf(points);
    const float length = arcs.back();
    const int count = std::max(static_cast<int>(std::lround(length / std::max(boneMetres, 1e-4f))), 1);
    const SwayChain chain{.firstBone = static_cast<int>(bones.size()), .boneCount = count, .length = length};

    for (int k = 0; k < count; ++k) {
        const auto [pivot, radius] = pointAlong(points, radii, arcs, length * static_cast<float>(k) / static_cast<float>(count));
        const Vec3 tip = pointAlong(points, radii, arcs, length * static_cast<float>(k + 1) / static_cast<float>(count)).first;
        bones.push_back(SwayBone{.pivot = pivot,
                                 .tip = tip,
                                 .parent = k == 0 ? parent : chain.firstBone + k - 1,
                                 .level = level,
                                 .radius = radius,
                                 .chainStart = chain.firstBone,
                                 .chainLength = length,
                                 .chainRadius = radii.front()});
    }

    return chain;
}

namespace {
    // The squared distance from a point to a line's closest point, and how far along the line that is.
    std::pair<float, float> closestOnLine(const std::vector<Vec3>& points, const Vec3& point) {
        float best = INFINITY;
        float bestArc = 0.0f;
        float arc = 0.0f;

        for (std::size_t k = 0; k + 1 < points.size(); ++k) {
            const Vec3 along = points[k + 1] - points[k];
            const float span = along.length();
            const float share = span > 0.0f ? std::clamp((point - points[k]).dot(along) / (span * span), 0.0f, 1.0f) : 0.0f;
            const float distance = (points[k] + along * share - point).lengthSquared();

            if (distance < best) {
                best = distance;
                bestArc = arc + share * span;
            }

            arc += span;
        }

        return {best, bestArc};
    }
}

float swayArcOfPoint(const std::vector<Vec3>& points, const Vec3& point) {
    return closestOnLine(points, point).second;
}

SwayLine makeSwayLine(const SwayChain& chain, const std::vector<Vec3>& points) {
    Vec3 low = points.front();
    Vec3 high = points.front();

    for (const Vec3& point : points) {
        low = Vec3{std::min(low.x, point.x), std::min(low.y, point.y), std::min(low.z, point.z)};
        high = Vec3{std::max(high.x, point.x), std::max(high.y, point.y), std::max(high.z, point.z)};
    }

    const Vec3 centre = (low + high) * 0.5f;
    float radius = 0.0f;

    for (const Vec3& point : points) {
        radius = std::max(radius, (point - centre).length());
    }

    return SwayLine{.chain = chain, .points = points, .centre = centre, .radius = radius};
}

SwayBinding swayNearestBinding(const std::vector<SwayLine>& lines, const std::vector<SwayBone>& bones, const Vec3& point, float blendShare) {
    if (lines.empty()) {
        throw std::invalid_argument("A point can only be bound to a skeleton with lines");
    }

    // A line whose sphere lies farther than the best line so far cannot be nearer: skipped.
    float best = INFINITY;
    const SwayLine* nearest = &lines.front();
    float nearestArc = 0.0f;

    for (const SwayLine& line : lines) {
        const float gap = std::max((point - line.centre).length() - line.radius, 0.0f);

        if (gap * gap >= best) {
            continue;
        }

        const auto [distance, arc] = closestOnLine(line.points, point);

        if (distance < best) {
            best = distance;
            nearest = &line;
            nearestArc = arc;
        }
    }

    return swayChainBinding(nearest->chain, bones, nearestArc, blendShare);
}

SwayBinding swayChainBinding(const SwayChain& chain, const std::vector<SwayBone>& bones, float arc, float blendShare) {
    const float boneLength = chain.length / static_cast<float>(std::max(chain.boneCount, 1));
    const float along = std::clamp(arc, 0.0f, chain.length);
    const int within = boneLength > 0.0f ? std::min(static_cast<int>(along / boneLength), chain.boneCount - 1) : 0;
    const int bone = chain.firstBone + within;

    if (bones[static_cast<std::size_t>(bone)].parent < 0) {
        return SwayBinding{.bone = bone, .weight = 1.0f};
    }

    const float fromPivot = boneLength > 0.0f ? along / boneLength - static_cast<float>(within) : 1.0f;
    return SwayBinding{.bone = bone, .weight = blendShare > 0.0f ? std::clamp(fromPivot / blendShare, 0.0f, 1.0f) : 1.0f};
}

bool swayBonesOrdered(const std::vector<SwayBone>& bones) {
    for (std::size_t k = 0; k < bones.size(); ++k) {
        if (bones[k].parent >= static_cast<int>(k)) {
            return false;
        }
    }

    return true;
}
