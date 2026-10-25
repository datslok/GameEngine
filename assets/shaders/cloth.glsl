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

struct ClothTick {
    float substepSeconds;
    float gravity;
    float stretchCompliance;
    float bendingStiffness;

    float bendPerSecondSquared;
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

// One hinge's isometric XPBD step (solveBend): one curvature vector and one multiplier, no angles or square roots.
void clothIsometricBend(inout vec4 wing0, inout vec4 wing1, inout vec4 edgeStart, inout vec4 edgeEnd, vec4 weights, float bendingStiffness,
                        float stiffnessShare, float perSecondSquared) {
    float moving = wing0.w * weights.x * weights.x;
    moving += wing1.w * weights.y * weights.y;
    moving += edgeStart.w * weights.z * weights.z;
    moving += edgeEnd.w * weights.w * weights.w;
    vec3 curvature = vec3(0.0) + wing0.xyz * weights.x;
    curvature = curvature + wing1.xyz * weights.y;
    curvature = curvature + edgeStart.xyz * weights.z;
    curvature = curvature + edgeEnd.xyz * weights.w;
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
