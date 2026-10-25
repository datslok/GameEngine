// A point's force and block summed by CLOTH_POINT_LANES invocations together (vertex block descent's sweep and the
// levels' gradient): each lane adds every CLOTH_POINT_LANES-th of the point's terms, then the lanes add their sums up by
// subgroup shuffles. A thread per point walked about 26 terms one after another, each waiting on reads that wait on
// reads, with too few points in a colour to hide the waits; eight lanes make each chain about eight times shorter and
// put eight times as many invocations in flight. The including pass enables GL_KHR_shader_subgroup_basic and
// GL_KHR_shader_subgroup_shuffle (the device must have shuffles in compute and subgroups of at least
// CLOTH_POINT_LANES, render/vulkan/vulkan_cloth.cpp).

#ifndef CLOTH_POINT_LANES_GLSL
#define CLOTH_POINT_LANES_GLSL

#include "cloth_pass.glsl"

/*
* This invocation's place among the dispatch's range of points (first, count) and its lane, false past the range. The
* lanes of one point are CLOTH_POINT_LANES neighbours in one subgroup, found from the subgroup's own numbering (Vulkan
* does not promise how a workgroup's invocations split into subgroups), so the shuffles stay among them; every
* subgroup is full, since its size divides CLOTH_GROUP_SIZE.
*/
bool clothPointLane(out uint index, out uint lane) {
    uint pointsPerSubgroup = gl_SubgroupSize / CLOTH_POINT_LANES;
    uint pointInGroup = gl_SubgroupID * pointsPerSubgroup + gl_SubgroupInvocationID / CLOTH_POINT_LANES;
    uint pointInDispatch = gl_WorkGroupID.x * (CLOTH_GROUP_SIZE / CLOTH_POINT_LANES) + pointInGroup;
    lane = gl_SubgroupInvocationID % CLOTH_POINT_LANES;
    index = first + pointInDispatch;
    return pointInDispatch < count;
}

/*
* The sum of a value over a point's lanes, by a butterfly: each lane adds the value of the lane CLOTH_POINT_LANES / 2
* away, then half that, down to 1. Two lanes paired at a step add the same two numbers (a + b and b + a, the same bits),
* so every lane ends with the same sum, in the same order every run, with no shared memory or barriers.
*/
vec3 clothLaneSum(vec3 value) {
    for (uint offset = CLOTH_POINT_LANES / 2u; offset > 0u; offset /= 2u) {
        value = value + subgroupShuffleXor(value, offset);
    }
    return value;
}

/*
* clothPointForce for a free point: minus phi's gradient at it, and its 3 x 3 block of phi's second derivatives, from
* cloth.glsl's terms: the inertia, its edges, its hinge corners (when the fabric bends), the hand, term t taken by lane
* t % CLOTH_POINT_LANES. The same terms as the CPU's, added in a different order (each lane's in turn, then the lanes'
* as a tree), so the same to within rounding. Every lane of the point must call it.
*/
vec3 clothPointForceLanes(uint point, uint lane, ClothTick tick, out ClothBlock block) {
    vec4 here = positions[point];
    block = clothEmptyBlock();
    vec3 force = vec3(0.0);

    uint edgeFirst = edgeStarts[point];
    uint edgeCount = edgeStarts[point + 1u] - edgeFirst;
    uint hingeFirst = hingeStarts[point];
    uint hingeCount = tick.bendingStiffness > 0.0 ? hingeStarts[point + 1u] - hingeFirst : 0u;
    uint termCount = 1u + edgeCount + hingeCount + (point == tick.grabParticle ? 1u : 0u);

    for (uint term = lane; term < termCount; term += CLOTH_POINT_LANES) {
        if (term == 0u) {
            clothInertiaTerm(force, block, here.xyz, predicted[point].xyz, tick.perSecondSquared / here.w);
        } else if (term <= edgeCount) {
            ClothPointEdge edge = pointEdges[edgeFirst + term - 1u];
            clothEdgeTerm(force, block, here.xyz, positions[edge.other].xyz, edge.restLength, tick.stretchStiffness);
        } else if (term <= edgeCount + hingeCount) {
            ClothPointHinge hinge = pointHinges[hingeFirst + term - 1u - edgeCount];
            float stiffness = tick.bendingStiffness * hinge.stiffnessShare;
            vec3 wing0 = positions[hinge.points.x].xyz;
            vec3 wing1 = positions[hinge.points.y].xyz;
            vec3 edgeStart = positions[hinge.points.z].xyz;
            vec3 edgeEnd = positions[hinge.points.w].xyz;
            if (tick.bendingModel == CLOTH_BEND_ISOMETRIC) {
                vec3 curvature = clothIsometricCurvature(hinge.isometricWeights, wing0, wing1, edgeStart, edgeEnd);
                clothIsometricHingeTerm(force, block, curvature, hinge.isometricWeights[hinge.corner], stiffness);
            } else {
                ClothHinge state = clothHingeState(wing0, wing1, edgeStart, edgeEnd);
                clothHingeAngleTerm(force, block, state.gradient[hinge.corner], clothWrapAngle(state.angle - hinge.restAngle), stiffness);
            }
        } else {
            clothHandTerm(force, block, here.xyz, tick.grabTarget.xyz, tick.grabStiffness);
        }
    }

    block.diagonal = clothLaneSum(block.diagonal);
    block.offDiagonal = clothLaneSum(block.offDiagonal);
    return clothLaneSum(force);
}

#endif
