// What every cloth pass (cloth_*.comp) binds, one layout for all of them (render/vulkan/vulkan_cloth.h): the cloth's
// points and constraints, the frame's ticks with their pins and bodies, vertex block descent's prediction, per-point
// lists and point colours, the tethers, and push constants saying which tick and substep this dispatch is and which range of points
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

// Each point's edges (ClothPointConstraints): from edgeStarts[point] to edgeStarts[point + 1] in pointEdges, each
// entry holding what the edge's term needs.
layout(std430, set = 0, binding = 10) readonly buffer ClothEdgeStarts {
    uint edgeStarts[];
};

layout(std430, set = 0, binding = 11) readonly buffer ClothPointEdges {
    ClothPointEdge pointEdges[];
};

// Each point's hinge corners, the same way.
layout(std430, set = 0, binding = 12) readonly buffer ClothHingeStarts {
    uint hingeStarts[];
};

layout(std430, set = 0, binding = 13) readonly buffer ClothPointHinges {
    ClothPointHinge pointHinges[];
};

// The points sorted by colour (ClothVertexColours::order): a vertex block descent dispatch covers one colour's range.
layout(std430, set = 0, binding = 14) readonly buffer ClothVertexOrder {
    uint vertexOrder[];
};

// Each point's tether slots (CLOTH_TETHERS_PER_POINT from point * CLOTH_TETHERS_PER_POINT), nearest anchor first, the
// spare ones last.
layout(std430, set = 0, binding = 15) readonly buffer ClothTethers {
    ClothTether tethers[];
};

// The frame's field bodies, from each tick's fieldFirst, and every field's samples, one field after another (x fastest,
// then y, then z), shared by every cloth.
layout(std430, set = 0, binding = 16) readonly buffer ClothFields {
    ClothField fields[];
};

layout(std430, set = 0, binding = 17) readonly buffer ClothFieldSamples {
    float fieldSamples[];
};

// Must match ClothPushConstants in render/vulkan/vulkan_cloth.cpp.
layout(push_constant) uniform ClothDispatch {
    uint tickIndex;
    uint step;  // the substep, 0..substeps - 1
    uint first; // the first point or constraint
    uint count;
    uint levelNumber;     // a coarse level's pass: which level, 1 the first coarser than the cloth
    uint clothPointCount; // the cloth's points, stretch constraints and hinges
    uint clothEdgeCount;
    uint clothHingeCount;
};

// sampleField and surfacePoint for a field body: its cell's eight corners read and blended; outside its box, a band away, up.
vec4 clothFieldSurfacePoint(ClothField field, vec3 point) {
    ivec3 cell;
    vec3 share;
    if (!clothFieldCell(point, field.origin.xyz, field.origin.w, field.counts.xyz, cell, share)) {
        return vec4(0.0, 1.0, 0.0, field.band.x);
    }
    uint row = field.counts.x;
    uint slice = field.counts.x * field.counts.y;
    uint first = field.counts.w + uint(cell.z) * slice + uint(cell.y) * row + uint(cell.x);
    float corners[8] = float[8](fieldSamples[first], fieldSamples[first + 1u], fieldSamples[first + row], fieldSamples[first + row + 1u],
                                fieldSamples[first + slice], fieldSamples[first + slice + 1u], fieldSamples[first + slice + row],
                                fieldSamples[first + slice + row + 1u]);
    return clothFieldSurface(clothFieldBlend(corners, share, field.origin.w));
}

// This invocation's place in the range, or false past its end (the last workgroup's spare invocations).
bool clothItem(out uint index) {
    index = first + gl_GlobalInvocationID.x;
    return gl_GlobalInvocationID.x < count;
}

#endif
