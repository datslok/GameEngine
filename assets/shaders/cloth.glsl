// The cloth solver on the GPU: the CPU solver's steps (src/cloth) ported as they are, one point or one constraint at a
// time, so the passes (cloth_*.comp) can run a whole colour at once. Pure functions, mirrored by the CPU solver itself
// and checked against it (tests/gpu/cloth_gpu_test.cpp). A point is a vec4: its position, and its inverse mass in w
// (0 pinned), since every step that moves a point weighs it.

#ifndef CLOTH_GLSL
#define CLOTH_GLSL

#include "cloth/cloth_shared.h"
#include "math/closest_points_shared.h"
#include "render/gpu/cloth_gpu_shared.h"

// The records the passes read, std430; their C++ mirrors are in render/gpu/cloth_gpu.h.
struct ClothStretch {
    uint first;
    uint second;
    float restLength;
    float unused;
};

struct ClothBend {
    uvec4 points; // wing0, wing1, edgeStart, edgeEnd
    float restAngle;
    float stiffnessShare;
    vec2 unused;
    vec4 isometricWeights;
};

struct ClothCollider {
    uint kind;
    float radius;
    vec2 unused;
    vec4 a;
    vec4 b;
};

struct ClothPin {
    vec4 start;
    vec4 target;
};

// One of a point's edges and one of its hinge corners, as its per-point lists hold them (GpuPointEdge, GpuPointHinge).
struct ClothPointEdge {
    uint other;
    float restLength;
};

struct ClothPointHinge {
    uvec4 points; // wing0, wing1, edgeStart, edgeEnd
    vec4 isometricWeights;
    float restAngle;
    float stiffnessShare;
    uint corner; // which of the points it is
    uint unused;
};

// A point on another level and its weight (render/gpu/cloth_gpu.h's GpuClothLink).
struct ClothLink {
    uint point;
    float weight;
};

// Where one coarse level's lists start (GpuClothLevel).
struct ClothLevel {
    uint pointsFirst;
    uint pointCount;
    uint neighbourStartsFirst;
    uint gatherStartsFirst;
    uint spreadStartsFirst;
    uint unused0;
    uint unused1;
    uint unused2;
};

struct ClothTick {
    float substepSeconds;
    float gravity;
    float stretchCompliance;
    float bendingStiffness;

    float perSecondSquared;
    float grabCompliance;
    float kept;
    float thickness;

    float staticFriction;
    float kineticFriction;
    uint bendingModel;
    uint substeps;

    uint pinFirst;
    uint colliderFirst;
    uint colliderCount;
    uint grabParticle;

    vec4 grabTarget;

    float stretchStiffness;
    float grabStiffness;
    uint method;
    uint unused;
};

const float CLOTH_FULL_TURN = 6.28318530717958647692;

// predictCloth for one point: a free point's velocity gains gain, then it moves by its new velocity; its start is kept.
void clothPredict(inout vec4 point, inout vec3 velocity, out vec3 previous, vec3 gain, float seconds) {
    previous = point.xyz;
    if (point.w > 0.0) {
        velocity = velocity + gain;
        point.xyz = point.xyz + velocity * seconds;
    }
}

// Where a moving pin is after substep step of substeps: slid evenly from its start, exactly on its target at the end.
vec3 clothPinPosition(ClothPin pin, uint step, uint substeps) {
    if (step + 1u == substeps) {
        return pin.target.xyz;
    }
    float share = float(step + 1u) / float(substeps);
    return pin.start.xyz + (pin.target.xyz - pin.start.xyz) * share;
}

// One edge's XPBD step (solveStretch), scaledCompliance the compliance over the step's length squared.
void clothStretch(inout vec4 first, inout vec4 second, float restLength, float scaledCompliance) {
    float weights = first.w + second.w;
    if (weights == 0.0) {
        return;
    }

    vec3 between = first.xyz - second.xyz;
    float edgeLength = length(between);
    if (edgeLength < CLOTH_SHORTEST_EDGE) {
        return;
    }

    vec3 along = between * (1.0 / edgeLength);
    float multiplier = -(edgeLength - restLength) / (weights + scaledCompliance);
    first.xyz = first.xyz + along * (first.w * multiplier);
    second.xyz = second.xyz - along * (second.w * multiplier);
}

// A hinge's curvature vector (isometricCurvature): its points weighted by the hinge's isometric weights.
vec3 clothIsometricCurvature(vec4 weights, vec3 wing0, vec3 wing1, vec3 edgeStart, vec3 edgeEnd) {
    vec3 curvature = vec3(0.0) + wing0 * weights.x;
    curvature = curvature + wing1 * weights.y;
    curvature = curvature + edgeStart * weights.z;
    return curvature + edgeEnd * weights.w;
}

// One hinge's isometric XPBD step (solveBend): one curvature vector and one multiplier, no angles or square roots.
void clothIsometricBend(inout vec4 wing0, inout vec4 wing1, inout vec4 edgeStart, inout vec4 edgeEnd, vec4 weights, float bendingStiffness,
                        float stiffnessShare, float perSecondSquared) {
    float moving = wing0.w * weights.x * weights.x;
    moving += wing1.w * weights.y * weights.y;
    moving += edgeStart.w * weights.z * weights.z;
    moving += edgeEnd.w * weights.w * weights.w;
    vec3 curvature = clothIsometricCurvature(weights, wing0.xyz, wing1.xyz, edgeStart.xyz, edgeEnd.xyz);
    if (moving == 0.0) {
        return;
    }

    float scaledCompliance = perSecondSquared / (bendingStiffness * stiffnessShare);
    vec3 multiplier = curvature * (-1.0 / (moving + scaledCompliance));
    wing0.xyz = wing0.xyz + multiplier * (wing0.w * weights.x);
    wing1.xyz = wing1.xyz + multiplier * (wing1.w * weights.y);
    edgeStart.xyz = edgeStart.xyz + multiplier * (edgeStart.w * weights.z);
    edgeEnd.xyz = edgeEnd.xyz + multiplier * (edgeEnd.w * weights.w);
}

// A hinge's angle and Bridson et al.'s gradient for each point (wing0, wing1, edgeStart, edgeEnd), as hingeState.
struct ClothHinge {
    float angle;
    vec3 gradient[4];
};

ClothHinge clothHingeState(vec3 wing0, vec3 wing1, vec3 edgeStart, vec3 edgeEnd) {
    ClothHinge hinge;
    hinge.angle = 0.0;
    hinge.gradient[0] = vec3(0.0);
    hinge.gradient[1] = vec3(0.0);
    hinge.gradient[2] = vec3(0.0);
    hinge.gradient[3] = vec3(0.0);

    vec3 normal0 = cross(wing0 - edgeStart, wing0 - edgeEnd);
    vec3 normal1 = cross(wing1 - edgeEnd, wing1 - edgeStart);
    vec3 edge = edgeEnd - edgeStart;
    if (dot(edge, edge) < CLOTH_SQUASHED_HINGE || dot(normal0, normal0) < CLOTH_SQUASHED_HINGE || dot(normal1, normal1) < CLOTH_SQUASHED_HINGE) {
        return hinge;
    }

    float edgeLength = length(edge);
    vec3 along = edge * (1.0 / edgeLength);
    vec3 scaled0 = normal0 * (1.0 / dot(normal0, normal0));
    vec3 scaled1 = normal1 * (1.0 / dot(normal1, normal1));

    hinge.angle = atan(dot(cross(normal1, normal0), along), dot(normal0, normal1));
    hinge.gradient[0] = scaled0 * edgeLength;
    hinge.gradient[1] = scaled1 * edgeLength;
    hinge.gradient[2] = scaled0 * dot(wing0 - edgeEnd, along) + scaled1 * dot(wing1 - edgeEnd, along);
    hinge.gradient[3] = (scaled0 * dot(wing0 - edgeStart, along) + scaled1 * dot(wing1 - edgeStart, along)) * -1.0;
    return hinge;
}

// wrapAngle: to -pi..pi, the nearest whole turns taken off (std::remainder's rounding, halves to even).
float clothWrapAngle(float radians) {
    return radians - CLOTH_FULL_TURN * roundEven(radians / CLOTH_FULL_TURN);
}

// One hinge's XPBD step on its angle (solveBend's hinge model).
void clothHingeBend(inout vec4 wing0, inout vec4 wing1, inout vec4 edgeStart, inout vec4 edgeEnd, float restAngle, float bendingStiffness,
                    float stiffnessShare, float perSecondSquared) {
    ClothHinge hinge = clothHingeState(wing0.xyz, wing1.xyz, edgeStart.xyz, edgeEnd.xyz);

    float moving = wing0.w * dot(hinge.gradient[0], hinge.gradient[0]);
    moving += wing1.w * dot(hinge.gradient[1], hinge.gradient[1]);
    moving += edgeStart.w * dot(hinge.gradient[2], hinge.gradient[2]);
    moving += edgeEnd.w * dot(hinge.gradient[3], hinge.gradient[3]);
    if (moving == 0.0) {
        return;
    }

    float scaledCompliance = perSecondSquared / (bendingStiffness * stiffnessShare);
    float error = clothWrapAngle(hinge.angle - restAngle);
    float multiplier = -error / (moving + scaledCompliance);
    wing0.xyz = wing0.xyz + hinge.gradient[0] * (wing0.w * multiplier);
    wing1.xyz = wing1.xyz + hinge.gradient[1] * (wing1.w * multiplier);
    edgeStart.xyz = edgeStart.xyz + hinge.gradient[2] * (edgeStart.w * multiplier);
    edgeEnd.xyz = edgeEnd.xyz + hinge.gradient[3] * (edgeEnd.w * multiplier);
}

// closestPointOnSegment (math/closest_points).
vec3 clothClosestOnSegment(vec3 point, vec3 a, vec3 b) {
    vec3 direction = b - a;
    float lengthSquared = dot(direction, direction);
    if (lengthSquared <= CLOSEST_POINTS_DEGENERATE_LENGTH_SQUARED) {
        return a;
    }
    float t = clamp(dot(point - a, direction) / lengthSquared, 0.0, 1.0);
    return a + direction * t;
}

// Out of a ball round a centre: the outward normal in xyz, the distance from its surface (negative inside) in w.
vec4 clothOutOfBall(vec3 centre, float radius, vec3 point) {
    vec3 away = point - centre;
    float awayLength = length(away);
    if (awayLength * awayLength < CLOTH_NO_DIRECTION) {
        return vec4(0.0, 1.0, 0.0, -radius);
    }
    return vec4(away * (1.0 / awayLength), awayLength - radius);
}

// Outside a box from its closest point; inside through the nearest face, the first of -x, +x, -y, +y, -z, +z on a tie.
vec4 clothOutOfBox(vec3 minimum, vec3 maximum, vec3 point) {
    bool inside = all(greaterThanEqual(point, minimum)) && all(lessThanEqual(point, maximum));
    if (!inside) {
        vec3 away = point - clamp(point, minimum, maximum);
        float awayLength = length(away);
        return vec4(away * (1.0 / awayLength), awayLength);
    }

    float depths[6] = float[6](point.x - minimum.x, maximum.x - point.x, point.y - minimum.y, maximum.y - point.y, point.z - minimum.z,
                               maximum.z - point.z);
    int nearest = 0;
    for (int face = 1; face < 6; ++face) {
        if (depths[face] < depths[nearest]) {
            nearest = face;
        }
    }
    vec3 normal = vec3(0.0);
    normal[nearest / 2] = (nearest % 2 == 0) ? -1.0 : 1.0;
    return vec4(normal, -depths[nearest]);
}

// surfacePoint: the way out of a body (xyz) and the distance from its surface, negative inside (w).
vec4 clothSurfacePoint(ClothCollider collider, vec3 point) {
    if (collider.kind == CLOTH_COLLIDER_SPHERE) {
        return clothOutOfBall(collider.a.xyz, collider.radius, point);
    }
    if (collider.kind == CLOTH_COLLIDER_CAPSULE) {
        return clothOutOfBall(clothClosestOnSegment(point, collider.a.xyz, collider.b.xyz), collider.radius, point);
    }
    return clothOutOfBox(collider.a.xyz, collider.b.xyz, point);
}

// One body's contact with one free point (solveContacts): pushed out to the thickness, then positional Coulomb friction
// against its slide since previous, the point's place when the substep began.
void clothContact(inout vec3 position, vec3 previous, vec4 surface, float thickness, float staticFriction, float kineticFriction) {
    float push = thickness - surface.w;
    if (push <= 0.0) {
        return;
    }

    vec3 normal = surface.xyz;
    position = position + normal * push;

    vec3 moved = position - previous;
    vec3 slide = moved - normal * dot(normal, moved);
    float slideLength = length(slide);
    if (slideLength < staticFriction * push) {
        position = position - slide;
    } else if (slideLength > 0.0) {
        position = position - slide * min(kineticFriction * push / slideLength, 1.0);
    }
}

// The hand's XPBD step on its point (solveGrab), scaledCompliance the compliance over the step's length squared.
void clothGrab(inout vec4 point, vec3 target, float scaledCompliance) {
    float weight = point.w;
    if (weight == 0.0) {
        return;
    }

    vec3 away = point.xyz - target;
    float distance = length(away);
    if (distance < CLOTH_ON_TARGET) {
        return;
    }

    float multiplier = -distance / (weight + scaledCompliance);
    point.xyz = point.xyz + away * (weight * multiplier / distance);
}

// ClothStiffnessBlock: a symmetric 3 x 3 matrix, its diagonal (xx, yy, zz) and the rest (xy, xz, yz).
struct ClothBlock {
    vec3 diagonal;
    vec3 offDiagonal;
};

ClothBlock clothEmptyBlock() {
    ClothBlock block;
    block.diagonal = vec3(0.0);
    block.offDiagonal = vec3(0.0);
    return block;
}

void clothBlockAddDiagonal(inout ClothBlock block, float value) {
    block.diagonal = block.diagonal + vec3(value);
}

// value times v v^T.
void clothBlockAddOuter(inout ClothBlock block, vec3 v, float value) {
    block.diagonal = block.diagonal + value * v * v;
    block.offDiagonal = block.offDiagonal + value * v.xxy * v.yzz;
}

// ClothStiffnessBlock::solve: the block's inverse times force, by the adjugate; zero for a block too near singular.
vec3 clothBlockSolve(ClothBlock block, vec3 force) {
    float xx = block.diagonal.x;
    float yy = block.diagonal.y;
    float zz = block.diagonal.z;
    float xy = block.offDiagonal.x;
    float xz = block.offDiagonal.y;
    float yz = block.offDiagonal.z;
    float cofactorXX = yy * zz - yz * yz;
    float cofactorXY = xz * yz - xy * zz;
    float cofactorXZ = xy * yz - xz * yy;
    float determinant = xx * cofactorXX + xy * cofactorXY + xz * cofactorXZ;
    float size = max(xx, max(yy, zz));
    if (!(determinant > float(CLOTH_SINGULAR_BLOCK) * size * size * size)) {
        return vec3(0.0);
    }
    float cofactorYY = xx * zz - xz * xz;
    float cofactorYZ = xy * xz - xx * yz;
    float cofactorZZ = xx * yy - xy * xy;
    float scale = 1.0 / determinant;
    return vec3((cofactorXX * force.x + cofactorXY * force.y + cofactorXZ * force.z) * scale,
                (cofactorXY * force.x + cofactorYY * force.y + cofactorYZ * force.z) * scale,
                (cofactorXZ * force.x + cofactorYZ * force.y + cofactorZZ * force.z) * scale);
}

// The pieces of a point's force and block in vertex block descent (cloth_energy.h's add*Term), each added to what is
// there; clothPointForceLanes (cloth_point_lanes.glsl) adds clothPointForce's terms, split over a point's lanes.
void clothInertiaTerm(inout vec3 force, inout ClothBlock block, vec3 here, vec3 predicted, float inertia) {
    force = force + (predicted - here) * inertia;
    clothBlockAddDiagonal(block, inertia);
}

void clothEdgeTerm(inout vec3 force, inout ClothBlock block, vec3 here, vec3 other, float restLength, float stiffness) {
    vec3 between = here - other;
    float edgeLength = length(between);
    if (edgeLength < CLOTH_SHORTEST_EDGE) {
        return;
    }
    vec3 along = between * (1.0 / edgeLength);
    force = force - along * (stiffness * (edgeLength - restLength));
    float across = max(0.0, 1.0 - restLength / edgeLength);
    clothBlockAddDiagonal(block, stiffness * across);
    clothBlockAddOuter(block, along, stiffness * (1.0 - across));
}

void clothIsometricHingeTerm(inout vec3 force, inout ClothBlock block, vec3 curvature, float weight, float stiffness) {
    force = force - curvature * (stiffness * weight);
    clothBlockAddDiagonal(block, stiffness * weight * weight);
}

void clothHingeAngleTerm(inout vec3 force, inout ClothBlock block, vec3 slope, float error, float stiffness) {
    force = force - slope * (stiffness * error);
    clothBlockAddOuter(block, slope, stiffness);
}

void clothHandTerm(inout vec3 force, inout ClothBlock block, vec3 here, vec3 target, float stiffness) {
    force = force - (here - target) * stiffness;
    clothBlockAddDiagonal(block, stiffness);
}

// addCoarseEdgeTerm: a coarse edge's stiffness along its direction now (between, from the point to its neighbour).
void clothCoarseEdgeTerm(inout ClothBlock block, vec3 between, float stiffness) {
    float lengthSquared = dot(between, between);
    if (lengthSquared >= float(CLOTH_SHORTEST_EDGE) * float(CLOTH_SHORTEST_EDGE)) {
        clothBlockAddOuter(block, between, stiffness / lengthSquared);
    }
}

// The line search's curvature terms d . H d (cloth_energy.h's inertiaCurvature and the rest), in float where the CPU
// adds them in double, and its step.
float clothInertiaCurvature(vec3 direction, float perSecondSquared, float inverseMass) {
    return dot(direction, direction) * perSecondSquared / inverseMass;
}

float clothEdgeCurvature(vec3 between, vec3 apart, float restLength, float stiffness) {
    float edgeLength = length(between);
    if (edgeLength < CLOTH_SHORTEST_EDGE) {
        return 0.0;
    }
    float along = dot(apart, between) / edgeLength;
    float across = max(0.0, dot(apart, apart) - along * along);
    float stretched = max(0.0, 1.0 - restLength / edgeLength);
    return stiffness * (along * along + stretched * across);
}

float clothIsometricHingeCurvature(vec4 weights, vec3 wing0, vec3 wing1, vec3 edgeStart, vec3 edgeEnd, float stiffness) {
    vec3 curvature = clothIsometricCurvature(weights, wing0, wing1, edgeStart, edgeEnd);
    return stiffness * dot(curvature, curvature);
}

// From the hinge's slopes of its angle (clothHingeState's gradient) and its corners' directions.
float clothHingeAngleCurvature(ClothHinge hinge, vec3 wing0, vec3 wing1, vec3 edgeStart, vec3 edgeEnd, float stiffness) {
    float change = 0.0;
    change += dot(hinge.gradient[0], wing0);
    change += dot(hinge.gradient[1], wing1);
    change += dot(hinge.gradient[2], edgeStart);
    change += dot(hinge.gradient[3], edgeEnd);
    return stiffness * change * change;
}

float clothHandCurvature(vec3 direction, float stiffness) {
    return stiffness * dot(direction, direction);
}

// lineSearchStep: -slope / curvature, or 0 unless the direction goes downhill into phi curving up.
float clothLineSearchStep(float slope, float curvature) {
    if (!(slope < 0.0) || !(curvature > 0.0)) {
        return 0.0;
    }
    return -slope / curvature;
}

// finishClothStep for one point, then the air's drag on a free point.
vec3 clothFinishVelocity(vec4 point, vec3 previous, float seconds, float kept) {
    float perSecond = 1.0 / seconds;
    vec3 velocity = (point.xyz - previous) * perSecond;
    if (point.w > 0.0) {
        velocity = velocity * kept;
    }
    return velocity;
}

#endif
