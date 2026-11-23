#pragma once

#include "core/settings_visitor.h"
#include "math/quat.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "scene/sway_bones.h"

#include <cstdint>
#include <vector>

/*
* Trees moved by bones (ROADMAP phase 21 stage 3c-ii, docs/research/LEAFGEOMETRY.md, Bone motion), mirrored by
* wind_bones.glsl and tested against it. Each bone turns about its pivot by its own small turn, worked out from its
* rest direction alone (so every bone's turn is independent), then carries its children: poses are composed parents
* first. A vertex follows its own bone blended with that bone's parent (two-bone blending, GPU Gems 3 ch. 6), so joints
* never open, and a cluster's bound is its sphere posed by its bones, never padded for the wind. Everything is in one
* frame, in metres: the tree's, with its lean given in it.
*/

/*
* How the bones move (WindSettings::bones, F5 Wind, Bones). A bone's turn grows as (foot radius / its radius)^stiffnessExponent
* (a cantilever's bend: drag a metre grows with r, stiffness with r^4), held under mostStiffness times the trunk's;
* a branch swings at its first mode, woodSpeed (sqrt(E / rho), green wood about 3000 m/s) times radius / 2 over its
* length squared times 1.875^2 / (2 pi), held between slowestHertz and fastestHertz (leaves' mass slows twigs); it swings
* by swingShare of its lean along the wind and sideShare across it, a quarter turn apart (an ellipse).
*/
struct WindBoneMotion {
    float stiffnessExponent = 3.0f;
    float mostStiffness = 8.0f;
    float woodSpeed = 3000.0f;
    float slowestHertz = 0.2f;
    float fastestHertz = 3.0f;
    float swingShare = 0.3f;
    float sideShare = 0.3f;

    // The bones the bones pass may pose a frame at first (two poses each, this frame's and last's, 32 B each: 256K
    // bones 16 MB). When a frame asks for more, the pool grows to the next power of two at or above the ask
    // (boneGrownPool), never past poolMostBones (4M bones, 256 MB); a tree past the pool stands still meanwhile, F4's
    // BONES line showing how many were asked for. Changing either starts again from poolBones.
    int poolBones = 262144;
    int poolMostBones = 4194304;
};

void visit(WindBoneMotion& motion, SettingsVisitor& visitor);

// Throws std::invalid_argument for an exponent below 0, a most stiffness below 1, a wood speed not above 0, a slowest
// hertz not above 0 or above the fastest, or a share below 0.
void validateWindBoneMotion(const WindBoneMotion& motion);

/*
* How much of the tree's lean a bone turns by: 2 boneLength / treeHeight (an untapered trunk then bends as the trees'
* parabola, windSwayOffset's, its tangent 2 lean h / H) times (footRadius / radius)^exponent, held under most.
*/
float windBoneShare(float boneLength, float treeHeight, float radius, float footRadius, float exponent, float most);

// A branch's (a chain's) swing (Hz): its first mode as WindBoneMotion says, held within [slowest, fastest].
float windBranchHertz(float length, float baseRadius, float woodSpeed, float slowest, float fastest);

// A branch's phase (radians, from 0 below 2 pi): its tree's seed and its chain's first bone hashed (pcg3d).
float windBranchPhase(std::uint32_t treeSeed, std::uint32_t chainStart);

/*
* A bone's own turn (a rotation vector, radians) in the tree's frame: direction its rest direction (unit), share
* windBoneShare's, lean the tree's lean (x and z, radians), hertz and phase its branch's, seconds the field's time,
* shares (swingShare, sideShare). share (d x W)(1 + swing sin t) + share side (W - d (d.W)) cos t for W the lean as a
* vector along the ground and t = 2 pi hertz seconds + phase: towards the wind by its share across the bone, and across
* both. Nothing in still air.
*/
Vec3 windBoneTurn(const Vec3& direction, float share, const Vec2& lean, float hertz, float phase, float seconds, const Vec2& shares);

// Where a bone carries things: T(x) = turn x + shift. The rest pose moves nothing.
struct WindBonePose {
    Quat turn;
    Vec3 shift;
};

/*
* A bone's pose from its parent's (the rest pose for a root), its own turn (windBoneTurn's) and its pivot: turned about
* its pivot, then carried by its parent, T(x) = T_parent(pivot) + (q_parent Q) (x - pivot), so its pivot stays where its
* parent carries it.
*/
WindBonePose windBonePose(const WindBonePose& parent, const Vec3& turn, const Vec3& pivot);

Vec3 windBonePlace(const WindBonePose& pose, const Vec3& place);

// A vertex bound to a bone (weight its own, the rest its parent's): the two bones' images blended.
Vec3 windBlendedPlace(const WindBonePose& own, const WindBonePose& parent, float weight, const Vec3& place);

// A normal or tangent bound likewise: the two turns of it blended, unit length again.
Vec3 windBlendedDirection(const WindBonePose& own, const WindBonePose& parent, float weight, const Vec3& direction);

/*
* A rigid thing bound at its foot (a twig: weight its own bone's share, the rest its parent's) carried whole: its turn
* the two turns blended (nlerp, the short way: a sway's turns are small, so nearly slerp), its foot where a vertex bound
* there blends to, so it stays on the bark it grows from.
*/
WindBonePose windRigidPose(const WindBonePose& own, const WindBonePose& parent, float weight, const Vec3& foot);

/*
* How a world point on a placed plant's bones moves (a liana's hold on its tree): in the plant's mesh frame
* (swayPlantPoint), blended by its two poses (windBlendedPlace), the move scaled and turned back into the world, as the
* plant's own vertex there moves.
*/
Vec3 windPlantPointOffset(const WindBonePose& own, const WindBonePose& parent, float weight, const Vec3& world, const Vec3& foot, float scale,
                          float turnRadians);

// A sphere (xyz its centre, w its radius) carried by a pose: moved, its radius kept.
Vec4 windPosedSphere(const WindBonePose& pose, const Vec4& sphere);

// The smallest sphere holding two spheres (xyz centre, w radius).
Vec4 windSphereUnion(const Vec4& first, const Vec4& second);

// A bone as its turn needs it, in its mesh's units: pivot, tip, radius at the pivot, its chain's first bone, length and
// radius at its start.
struct WindBoneShape {
    Vec3 pivot;
    Vec3 tip;
    float radius = 0.0f;
    std::uint32_t chainStart = 0;
    float chainLength = 0.0f;
    float chainRadius = 0.0f;
};

// Its tree: height and foot radius in the mesh's units, metres a unit (the plant's scale), seed (its branches' phases).
struct WindTreeShape {
    float height = 1.0f;
    float footRadius = 0.0f;
    float metresPerUnit = 1.0f;
    std::uint32_t seed = 0;
};

/*
* A bone's own turn in its tree: windBoneTurn of its rest direction (up for a bone of no length), its share
* (windBoneShare: lengths and radii in mesh units, so the scale cancels) and its branch's hertz (its chain's length and
* radius in metres) and phase.
*/
Vec3 windBoneOwnTurn(const WindBoneShape& bone, const WindTreeShape& tree, const Vec2& lean, float seconds, const WindBoneMotion& motion);

/*
* Every bone's pose (in their order, parents first: swayBonesOrdered) for a tree of a height (mesh units), metres a
* unit and seed with a lean at a time; its foot radius the first bone's. In list order on the CPU: the reference the
* bones pass's level by level posing (boneTreePoses) is tested against.
*/
std::vector<WindBonePose> windTreePoses(const std::vector<SwayBone>& bones, float treeHeight, float metresPerUnit, std::uint32_t treeSeed, const Vec2& lean,
                                        float seconds, const WindBoneMotion& motion);
