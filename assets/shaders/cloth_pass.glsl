// What every cloth pass (cloth_*.comp) binds, one layout for all of them (render/vulkan/vulkan_cloth.h): the cloth's
// points and constraints, the frame's ticks with their pins and bodies, vertex block descent's prediction, per-point
// lists and point colours, and push constants saying which tick and substep this dispatch is and which range of points
// or constraints it covers. One invocation per point or constraint.

#ifndef CLOTH_PASS_GLSL
#define CLOTH_PASS_GLSL

#include "cloth.glsl"

layout(local_size_x = CLOTH_GROUP_SIZE) in;

layout(std430, set = 0, binding = 0) buffer ClothPositions {
    vec4 positions[]; // inverse mass in w
};

layout(std430, set = 0, binding = 1) buffer ClothPrevious {
    vec4 previousPositions[];
};

layout(std430, set = 0, binding = 2) buffer ClothVelocities {
    vec4 velocities[];
};

layout(std430, set = 0, binding = 3) readonly buffer ClothStretches {
    ClothStretch stretches[];
};

layout(std430, set = 0, binding = 4) readonly buffer ClothBends {
    ClothBend bends[];
};

layout(std430, set = 0, binding = 5) readonly buffer ClothPinSlots {
    uint pinSlots[]; // CLOTH_NO_PIN for a point no pin moves
};

layout(std430, set = 0, binding = 6) readonly buffer ClothTicks {
    ClothTick ticks[];
};

layout(std430, set = 0, binding = 7) readonly buffer ClothPins {
    ClothPin pins[];
};

layout(std430, set = 0, binding = 8) readonly buffer ClothColliders {
    ClothCollider colliders[];
};

// Where momentum and gravity carry each point this substep (the prediction phi's inertia pulls towards).
layout(std430, set = 0, binding = 9) buffer ClothPredicted {
    vec4 predicted[];
};

// Each point's stretch constraints (ClothPointConstraints): from edgeStarts[point] to edgeStarts[point + 1] in pointEdges.
layout(std430, set = 0, binding = 10) readonly buffer ClothEdgeStarts {
    uint edgeStarts[];
};

layout(std430, set = 0, binding = 11) readonly buffer ClothPointEdges {
    uint pointEdges[];
};

// Each point's hinge corners: hinge index times 4 plus which corner it is (wing0, wing1, edgeStart, edgeEnd).
layout(std430, set = 0, binding = 12) readonly buffer ClothHingeStarts {
    uint hingeStarts[];
};

layout(std430, set = 0, binding = 13) readonly buffer ClothPointHinges {
    uint pointHinges[];
};

// The points sorted by colour (ClothVertexColours::order): a vertex block descent dispatch covers one colour's range.
layout(std430, set = 0, binding = 14) readonly buffer ClothVertexOrder {
    uint vertexOrder[];
};

// Must match ClothPushConstants in render/vulkan/vulkan_cloth.cpp.
layout(push_constant) uniform ClothDispatch {
    uint tickIndex;
    uint step;  // the substep, 0..substeps - 1
    uint first; // the first point or constraint
    uint count;
};

// This invocation's place in the range, or false past its end (the last workgroup's spare invocations).
bool clothItem(out uint index) {
    index = first + gl_GlobalInvocationID.x;
    return gl_GlobalInvocationID.x < count;
}

/*
* clothPointForce for a free point: minus phi's gradient at it, and its 3 x 3 block of phi's second derivatives, from
* its own lists, adding cloth.glsl's terms in the CPU's order: the inertia, its edges, its hinges (when the fabric
* bends), the hand.
*/
vec3 clothPointForceAt(uint point, ClothTick tick, out ClothBlock block) {
    vec4 here = positions[point];
    block = clothEmptyBlock();
    vec3 force = vec3(0.0);
    clothInertiaTerm(force, block, here.xyz, predicted[point].xyz, tick.perSecondSquared / here.w);

    for (uint at = edgeStarts[point]; at < edgeStarts[point + 1u]; ++at) {
        ClothStretch edge = stretches[pointEdges[at]];
        uint other = edge.first == point ? edge.second : edge.first;
        clothEdgeTerm(force, block, here.xyz, positions[other].xyz, edge.restLength, tick.stretchStiffness);
    }

    if (tick.bendingStiffness > 0.0) {
        for (uint at = hingeStarts[point]; at < hingeStarts[point + 1u]; ++at) {
            ClothBend bend = bends[pointHinges[at] / 4u];
            uint corner = pointHinges[at] % 4u;
            float stiffness = tick.bendingStiffness * bend.stiffnessShare;
            vec3 wing0 = positions[bend.points.x].xyz;
            vec3 wing1 = positions[bend.points.y].xyz;
            vec3 edgeStart = positions[bend.points.z].xyz;
            vec3 edgeEnd = positions[bend.points.w].xyz;
            if (tick.bendingModel == CLOTH_BEND_ISOMETRIC) {
                vec3 curvature = clothIsometricCurvature(bend.isometricWeights, wing0, wing1, edgeStart, edgeEnd);
                clothIsometricHingeTerm(force, block, curvature, bend.isometricWeights[corner], stiffness);
            } else {
                ClothHinge hinge = clothHingeState(wing0, wing1, edgeStart, edgeEnd);
                clothHingeAngleTerm(force, block, hinge.gradient[corner], clothWrapAngle(hinge.angle - bend.restAngle), stiffness);
            }
        }
    }

    if (point == tick.grabParticle) {
        clothHandTerm(force, block, here.xyz, tick.grabTarget.xyz, tick.grabStiffness);
    }
    return force;
}

#endif
