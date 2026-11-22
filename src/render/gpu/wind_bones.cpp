#include "render/gpu/wind_bones.h"

#include "core/hash.h"
#include "render/gpu/wind_bones_shared.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
    constexpr float firstMode = static_cast<float>(WIND_BONE_FIRST_MODE);
    constexpr float thinnest = static_cast<float>(WIND_BONE_THINNEST);
    constexpr float twoPi = 6.28318530718f;
}

void visit(WindBoneMotion& motion, SettingsVisitor& visitor) {
    visitor.field("Stiffness exponent", motion.stiffnessExponent, {.min = 0.0f, .max = 6.0f});
    visitor.field("Most stiffness", motion.mostStiffness, {.min = 1.0f, .max = 1000.0f, .sliderMax = 50.0f});
    visitor.field("Wood speed", motion.woodSpeed, {.min = 1.0f, .max = 10000.0f, .sliderMax = 6000.0f, .unit = "m/s"});
    visitor.field("Slowest branch", motion.slowestHertz, {.min = 0.01f, .max = 20.0f, .sliderMax = 2.0f, .unit = "Hz"});
    visitor.field("Fastest branch", motion.fastestHertz, {.min = 0.01f, .max = 20.0f, .sliderMax = 8.0f, .unit = "Hz"});
    visitor.field("Swing along", motion.swingShare, {.min = 0.0f, .max = 2.0f, .sliderMax = 1.0f});
    visitor.field("Swing across", motion.sideShare, {.min = 0.0f, .max = 2.0f, .sliderMax = 1.0f});
}

void validateWindBoneMotion(const WindBoneMotion& motion) {
    if (motion.stiffnessExponent < 0.0f || motion.mostStiffness < 1.0f || motion.woodSpeed <= 0.0f) {
        throw std::invalid_argument("bone stiffness exponent must be at least 0, most stiffness at least 1, wood speed above 0");
    }

    if (motion.slowestHertz <= 0.0f || motion.slowestHertz > motion.fastestHertz) {
        throw std::invalid_argument("branches' slowest swing must be above 0 and no faster than their fastest");
    }

    if (motion.swingShare < 0.0f || motion.sideShare < 0.0f) {
        throw std::invalid_argument("bones' swing shares must be at least 0");
    }
}

float windBoneShare(float boneLength, float treeHeight, float radius, float footRadius, float exponent, float most) {
    const float stiffness = std::min(std::pow(std::max(footRadius, thinnest) / std::max(radius, thinnest), exponent), most);
    return 2.0f * boneLength / std::max(treeHeight, 1e-4f) * stiffness;
}

float windBranchHertz(float length, float baseRadius, float woodSpeed, float slowest, float fastest) {
    const float hertz = firstMode * woodSpeed * 0.5f * baseRadius / std::max(length * length, 1e-8f);
    return std::clamp(hertz, slowest, fastest);
}

float windBranchPhase(std::uint32_t treeSeed, std::uint32_t chainStart) {
    const std::uint32_t hashed = pcg3d({treeSeed, chainStart, 0x9e3779b9u})[0];
    return static_cast<float>(hashed >> 8u) * (twoPi / 16777216.0f);
}

Vec3 windBoneTurn(const Vec3& direction, float share, const Vec2& lean, float hertz, float phase, float seconds, const Vec2& shares) {
    const Vec3 wind{lean.x, 0.0f, lean.y};
    const float turn = twoPi * hertz * seconds + phase;
    const Vec3 towards = direction.cross(wind) * (1.0f + shares.x * std::sin(turn));
    const Vec3 across = (wind - direction * direction.dot(wind)) * (shares.y * std::cos(turn));
    return (towards + across) * share;
}

WindBonePose windBonePose(const WindBonePose& parent, const Vec3& turn, const Vec3& pivot) {
    const Quat composed = parent.turn * Quat::fromRotationVector(turn);
    return WindBonePose{.turn = composed, .shift = windBonePlace(parent, pivot) - composed.rotate(pivot)};
}

Vec3 windBonePlace(const WindBonePose& pose, const Vec3& place) {
    return pose.turn.rotate(place) + pose.shift;
}

Vec3 windBlendedPlace(const WindBonePose& own, const WindBonePose& parent, float weight, const Vec3& place) {
    return windBonePlace(own, place) * weight + windBonePlace(parent, place) * (1.0f - weight);
}

Vec3 windBlendedDirection(const WindBonePose& own, const WindBonePose& parent, float weight, const Vec3& direction) {
    return (own.turn.rotate(direction) * weight + parent.turn.rotate(direction) * (1.0f - weight)).normalized();
}

Vec4 windPosedSphere(const WindBonePose& pose, const Vec4& sphere) {
    const Vec3 centre = windBonePlace(pose, Vec3{sphere.x, sphere.y, sphere.z});
    return Vec4{centre.x, centre.y, centre.z, sphere.w};
}

Vec4 windSphereUnion(const Vec4& first, const Vec4& second) {
    const Vec3 a{first.x, first.y, first.z};
    const Vec3 b{second.x, second.y, second.z};
    const float apart = (b - a).length();

    if (apart + second.w <= first.w) {
        return first;
    }

    if (apart + first.w <= second.w) {
        return second;
    }

    const float radius = 0.5f * (apart + first.w + second.w);
    const Vec3 centre = a + (b - a) * ((radius - first.w) / apart);
    return Vec4{centre.x, centre.y, centre.z, radius};
}

std::vector<WindBonePose> windTreePoses(const std::vector<SwayBone>& bones, float treeHeight, std::uint32_t treeSeed, const Vec2& lean,
                                        float seconds, const WindBoneMotion& motion) {
    std::vector<WindBonePose> poses;
    poses.reserve(bones.size());
    const float footRadius = bones.empty() ? 0.0f : bones[0].radius;
    const Vec2 shares{motion.swingShare, motion.sideShare};

    for (const SwayBone& bone : bones) {
        const Vec3 along = bone.tip - bone.pivot;
        const float length = along.length();
        const Vec3 direction = length > 0.0f ? along * (1.0f / length) : Vec3{0.0f, 1.0f, 0.0f};
        const float share = windBoneShare(length, treeHeight, bone.radius, footRadius, motion.stiffnessExponent, motion.mostStiffness);
        const float hertz = windBranchHertz(bone.chainLength, bone.chainRadius, motion.woodSpeed, motion.slowestHertz, motion.fastestHertz);
        const float phase = windBranchPhase(treeSeed, static_cast<std::uint32_t>(bone.chainStart));
        const Vec3 turn = windBoneTurn(direction, share, lean, hertz, phase, seconds, shares);
        const WindBonePose parent = bone.parent < 0 ? WindBonePose{} : poses[static_cast<std::size_t>(bone.parent)];
        poses.push_back(windBonePose(parent, turn, bone.pivot));
    }

    return poses;
}
