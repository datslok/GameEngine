// Trees moved by bones (render/gpu/wind_bones.h, which documents each function and mirrors it), included with
// #include "wind_bones.glsl". Everything in one frame, in metres: the tree's, its lean given in it.

#ifndef WIND_BONES_GLSL
#define WIND_BONES_GLSL

#include "hash.glsl"
#include "quat.glsl"
#include "render/gpu/wind_bones_shared.h"

// Where a bone carries things: T(x) = turn x + shift. Must match WindBonePose.
struct WindBonePose {
    vec4 turn;
    vec3 shift;
};

WindBonePose windRestPose() {
    return WindBonePose(vec4(0.0, 0.0, 0.0, 1.0), vec3(0.0));
}

// How much of the tree's lean a bone turns by (windBoneShare).
float windBoneShare(float boneLength, float treeHeight, float radius, float footRadius, float exponent, float most) {
    float thinnest = float(WIND_BONE_THINNEST);
    float stiffness = min(pow(max(footRadius, thinnest) / max(radius, thinnest), exponent), most);
    return 2.0 * boneLength / max(treeHeight, 1e-4) * stiffness;
}

// A branch's swing (Hz), its first mode held within [slowest, fastest] (windBranchHertz).
float windBranchHertz(float length, float baseRadius, float woodSpeed, float slowest, float fastest) {
    float hertz = float(WIND_BONE_FIRST_MODE) * woodSpeed * 0.5 * baseRadius / max(length * length, 1e-8);
    return clamp(hertz, slowest, fastest);
}

// A branch's phase (radians, from 0 below 2 pi) from its tree's seed and its chain's first bone (windBranchPhase).
float windBranchPhase(uint treeSeed, uint chainStart) {
    uint hashed = pcg3d(uvec3(treeSeed, chainStart, 0x9e3779b9u)).x;
    return float(hashed >> 8u) * (6.28318530718 / 16777216.0);
}

// A bone's own turn (a rotation vector, radians): towards the wind by its share across the bone, and across both (windBoneTurn).
vec3 windBoneTurn(vec3 direction, float share, vec2 lean, float hertz, float phase, float seconds, vec2 shares) {
    vec3 wind = vec3(lean.x, 0.0, lean.y);
    float turn = 6.28318530718 * hertz * seconds + phase;
    vec3 towards = cross(direction, wind) * (1.0 + shares.x * sin(turn));
    vec3 across = (wind - direction * dot(direction, wind)) * (shares.y * cos(turn));
    return (towards + across) * share;
}

/*
* A bone's own turn in its tree (windBoneOwnTurn): tree its height and foot radius (mesh units), metres a unit; motion
* the field's two words of WindBoneMotion (exponent, most, wood speed, slowest; fastest, swing along, swing across).
*/
vec3 windBoneOwnTurn(vec3 pivot, vec3 tip, float radius, uint chainStart, float chainLength, float chainRadius, vec3 tree, uint seed, vec2 lean,
                     float seconds, vec4 motion, vec4 swing) {
    vec3 along = tip - pivot;
    float boneLength = length(along);
    vec3 direction = boneLength > 0.0 ? along / boneLength : vec3(0.0, 1.0, 0.0);
    float share = windBoneShare(boneLength, tree.x, radius, tree.y, motion.x, motion.y);
    float hertz = windBranchHertz(chainLength * tree.z, chainRadius * tree.z, motion.z, motion.w, swing.x);
    float phase = windBranchPhase(seed, chainStart);
    return windBoneTurn(direction, share, lean, hertz, phase, seconds, swing.yz);
}

vec3 windBonePlace(WindBonePose pose, vec3 place) {
    return quatRotate(pose.turn, place) + pose.shift;
}

// A bone's pose from its parent's, its own turn and its pivot: turned about its pivot, then carried (windBonePose).
WindBonePose windBonePose(WindBonePose parent, vec3 turn, vec3 pivot) {
    vec4 composed = quatMultiply(parent.turn, quatFromRotationVector(turn));
    return WindBonePose(composed, windBonePlace(parent, pivot) - quatRotate(composed, pivot));
}

// A vertex bound to a bone (weight its own, the rest its parent's): the two images blended (windBlendedPlace).
vec3 windBlendedPlace(WindBonePose own, WindBonePose parent, float weight, vec3 place) {
    return windBonePlace(own, place) * weight + windBonePlace(parent, place) * (1.0 - weight);
}

// A normal or tangent bound likewise, unit length again (windBlendedDirection).
vec3 windBlendedDirection(WindBonePose own, WindBonePose parent, float weight, vec3 direction) {
    return normalize(quatRotate(own.turn, direction) * weight + quatRotate(parent.turn, direction) * (1.0 - weight));
}

// A world point in a plant's mesh frame: its placement (foot, turn about up, scale) undone (swayPlantPoint).
vec3 windPlantPoint(vec3 world, vec3 foot, float scale, float turnRadians) {
    vec3 apart = world - foot;
    float cosine = cos(turnRadians);
    float sine = sin(turnRadians);
    return vec3(cosine * apart.x - sine * apart.z, apart.y, sine * apart.x + cosine * apart.z) * (1.0 / scale);
}

// How a world point on a placed plant's bones moves: blended in its mesh frame, back in the world (windPlantPointOffset).
vec3 windPlantPointOffset(WindBonePose own, WindBonePose parent, float weight, vec3 world, vec3 foot, float scale, float turnRadians) {
    vec3 inPlant = windPlantPoint(world, foot, scale, turnRadians);
    vec3 moved = (windBlendedPlace(own, parent, weight, inPlant) - inPlant) * scale;
    float cosine = cos(turnRadians);
    float sine = sin(turnRadians);
    return vec3(cosine * moved.x + sine * moved.z, moved.y, cosine * moved.z - sine * moved.x);
}

// A rigid thing bound at its foot carried whole: the turns blended the short way, its foot blended (windRigidPose).
WindBonePose windRigidPose(WindBonePose own, WindBonePose parent, float weight, vec3 foot) {
    vec4 ownTurn = dot(own.turn, parent.turn) < 0.0 ? -own.turn : own.turn;
    vec4 turn = normalize(parent.turn * (1.0 - weight) + ownTurn * weight);
    return WindBonePose(turn, windBlendedPlace(own, parent, weight, foot) - quatRotate(turn, foot));
}

// A sphere (xyz centre, w radius) carried by a pose (windPosedSphere).
vec4 windPosedSphere(WindBonePose pose, vec4 sphere) {
    return vec4(windBonePlace(pose, sphere.xyz), sphere.w);
}

// How far a pose moves what it carries: x its pivot's or tip's larger move, y 2 |turn.xyz| (windPoseMoves).
vec2 windPoseMoves(WindBonePose pose, vec3 pivot, vec3 tip) {
    float ends = max(length(windBonePlace(pose, pivot) - pivot), length(windBonePlace(pose, tip) - tip));
    return vec2(ends, 2.0 * length(pose.turn.xyz));
}

// The farthest a tree moves anything within reach of its bones, from its bones' largest moves (windTreeMove).
float windTreeMove(vec2 largestMoves, float reach) {
    return largestMoves.x + largestMoves.y * reach;
}

// The smallest sphere holding two (windSphereUnion).
vec4 windSphereUnion(vec4 first, vec4 second) {
    float apart = length(second.xyz - first.xyz);

    if (apart + second.w <= first.w) {
        return first;
    }

    if (apart + first.w <= second.w) {
        return second;
    }

    float radius = 0.5 * (apart + first.w + second.w);
    return vec4(first.xyz + (second.xyz - first.xyz) * ((radius - first.w) / apart), radius);
}

// Where a sphere's points go blended between two poses: within a sphere holding both images of it (windBlendedSphere).
vec4 windBlendedSphere(WindBonePose own, WindBonePose parent, vec4 sphere) {
    return windSphereUnion(windPosedSphere(own, sphere), windPosedSphere(parent, sphere));
}

#endif
