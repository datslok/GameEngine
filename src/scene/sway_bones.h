#pragma once

#include "math/vec3.h"

#include <vector>

/*
* Bones a swaying thing is moved by (ROADMAP phase 21 stage 3c, docs/research/LEAFGEOMETRY.md, Wind with valid bounds):
* as Nanite Foliage's skinning and GPU Gems 3 ch. 6's branches, each bone turns about its pivot and carries its children,
* and a vertex follows its own bone blended with that bone's parent near the pivot (two-bone blending), so joints never
* open. Lines (a tree's branch chains) are cut into bones of one length each; their transforms are worked out once a
* frame (stage 3c-ii on), so culling bounds come from the bones, never padded for the wind.
*/
struct SwayBone {
    Vec3 pivot;          // where it turns: its start, on its parent
    Vec3 tip;            // its end
    int parent = -1;     // before it in the list; -1 for a root
    int level = 0;       // 0 for a trunk, 1 its branches, 2 theirs, ...
    float radius = 0.0f; // the line's radius at its pivot, for its stiffness
    // Its chain (branch): its first bone, its length and its radius at its start, for the branch's swing.
    int chainStart = 0;
    float chainLength = 0.0f;
    float chainRadius = 0.0f;
};

// How a vertex follows the bones: weight of its own bone, the rest its parent's (a root's weight is whole).
struct SwayBinding {
    int bone = 0;
    float weight = 1.0f;
};

// A line's bones: boneCount from firstBone on, each length / boneCount long along it.
struct SwayChain {
    int firstBone = 0;
    int boneCount = 0;
    float length = 0.0f;
};

/*
* A line (points, at least two, and the radius at each) cut into bones about boneMetres long along it, all one length
* (a whole number of them, at least one), appended to bones: each the one before's child, the first parent's, all of
* level; pivots and tips on the line, radii taken along it.
*/
SwayChain appendSwayChain(std::vector<SwayBone>& bones, const std::vector<Vec3>& points, const std::vector<float>& radii, int parent, int level,
                          float boneMetres);

// How far along a line (metres from its first point) its closest point to a point lies.
float swayArcOfPoint(const std::vector<Vec3>& points, const Vec3& point);

/*
* What a point arc metres along a chain follows: the bone it lies in (the first before the line, the last past it),
* weighted towards that bone's parent over the first blendShare of the bone from its pivot (0 at the pivot, so the bone
* before carries a joint alone, continuously). A bone without a parent takes its whole weight.
*/
SwayBinding swayChainBinding(const SwayChain& chain, const std::vector<SwayBone>& bones, float arc, float blendShare);

// Whether every bone's parent comes before it (so parents are worked out first).
bool swayBonesOrdered(const std::vector<SwayBone>& bones);

/*
* A swaying thing's skeleton as its parts carry it (RenderPart::skeleton, ROADMAP phase 21 stage 3c-iii): its bones,
* parents first, and its height in its mesh's units, which a bone's share of its lean is measured against
* (render/gpu/wind_bones.h, windBoneShare).
*/
struct SwaySkeleton {
    std::vector<SwayBone> bones;
    float height = 1.0f;
};

// A chain and the line it was cut from, with a sphere holding the line, for binding things near it (swayNearestBinding).
struct SwayLine {
    SwayChain chain;
    std::vector<Vec3> points;
    Vec3 centre;
    float radius = 0.0f;
};

// A line of a chain's points (at least two), its sphere about the middle of its box.
SwayLine makeSwayLine(const SwayChain& chain, const std::vector<Vec3>& points);

/*
* What a point near a skeleton follows: its nearest line's binding where its closest point lies along it
* (swayChainBinding), so a climber or a perched plant moves with the bark under it. Throws std::invalid_argument for no
* lines.
*/
SwayBinding swayNearestBinding(const std::vector<SwayLine>& lines, const std::vector<SwayBone>& bones, const Vec3& point, float blendShare);
