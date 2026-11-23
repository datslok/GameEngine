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

#endif
