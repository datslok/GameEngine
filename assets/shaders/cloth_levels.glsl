// What the coarse levels' passes (cloth_held, cloth_gradient, cloth_coarse_step, cloth_spread and the line search's
// cloth_line_terms, cloth_line_total and cloth_line_apply.comp) bind besides the cloth passes' set 0: set 1, the cloth's
// levels (render/gpu/cloth_gpu.h's GpuClothLevels, every level's lists one after another, a record per level) and the
// work arrays a level's step passes along, and a sum over a workgroup for the line search.

#ifndef CLOTH_LEVELS_GLSL
#define CLOTH_LEVELS_GLSL

#include "cloth_pass.glsl"

// phi's gradient at each point (zero at held points), the direction a level moves the cloth, each level point's step.
layout(std430, set = 1, binding = 0) buffer ClothGradient {
    vec4 gradient[];
};

layout(std430, set = 1, binding = 1) buffer ClothDirection {
    vec4 direction[];
};

layout(std430, set = 1, binding = 2) buffer ClothCoarseSteps {
    vec4 coarseSteps[];
};

// 1 for a point the levels leave alone this substep: pinned, or within the thickness of a body.
layout(std430, set = 1, binding = 3) buffer ClothHeld {
    uint held[];
};

layout(std430, set = 1, binding = 4) readonly buffer ClothLevelRecords {
    ClothLevel clothLevels[];
};

// Each level's points (the cloth's indices) and their regions' masses, from its record's pointsFirst.
layout(std430, set = 1, binding = 5) readonly buffer ClothLevelPoints {
    uint levelPoints[];
};

layout(std430, set = 1, binding = 6) readonly buffer ClothLevelMasses {
    float levelMasses[];
};

// Each level point's coarse neighbours (the cloth's indices).
layout(std430, set = 1, binding = 7) readonly buffer ClothLevelNeighbourStarts {
    uint levelNeighbourStarts[];
};

layout(std430, set = 1, binding = 8) readonly buffer ClothLevelNeighbours {
    uint levelNeighbours[];
};

// Each level point's column of P: the cloth points it moves and its weight for each.
layout(std430, set = 1, binding = 9) readonly buffer ClothGatherStarts {
    uint gatherStarts[];
};

layout(std430, set = 1, binding = 10) readonly buffer ClothGatherLinks {
    ClothLink gatherLinks[];
};

// Each cloth point's row of P: the level points that move it and their weights.
layout(std430, set = 1, binding = 11) readonly buffer ClothSpreadStarts {
    uint spreadStarts[];
};

layout(std430, set = 1, binding = 12) readonly buffer ClothSpreadLinks {
    ClothLink spreadLinks[];
};

// Each point's level: the coarsest holding it, 0 for a point on none.
layout(std430, set = 1, binding = 13) readonly buffer ClothPointLevels {
    uint pointLevels[];
};

// The line search: a (slope, curvature) sum per workgroup of its terms, then the step length they give.
layout(std430, set = 1, binding = 14) buffer ClothLinePartials {
    vec2 linePartials[];
};

layout(std430, set = 1, binding = 15) buffer ClothLineStep {
    float lineStep[];
};

shared vec2 clothGroupTerms[CLOTH_GROUP_SIZE];

/*
* The sum of every invocation's value in the workgroup, added as a tree in a fixed order (pairs, then pairs of pairs),
* so the same values always give the same bits whichever invocations run first. Every invocation of the group must
* call it, past the end of the range too (with nothing to add): barrier waits for all of them.
*/
vec2 clothGroupSum(vec2 value) {
    uint lane = gl_LocalInvocationID.x;
    clothGroupTerms[lane] = value;
    memoryBarrierShared();
    barrier();
    for (uint stride = CLOTH_GROUP_SIZE / 2u; stride > 0u; stride /= 2u) {
        if (lane < stride) {
            clothGroupTerms[lane] = clothGroupTerms[lane] + clothGroupTerms[lane + stride];
        }
        memoryBarrierShared();
        barrier();
    }
    return clothGroupTerms[0];
}

#endif
