// What the mesh cluster passes share (render/vulkan/vulkan_mesh_cluster_culling.h): the tables they read and the pair at
// a place in the work (mirrors meshClusterPairAt and cullMeshClustersOnCpu's test, render/gpu/mesh_cluster_culling.h).
// The passes that test pairs define MESH_CLUSTER_PYRAMID_BINDING (after their storage buffers) for the two pyramids.
#ifndef MESH_CLUSTER_PASS_GLSL
#define MESH_CLUSTER_PASS_GLSL

#include "mesh_cluster_culling.glsl"

// The camera's view as its culling placed it, with the wind's sway (its own copy: the camera culls without it).
layout(std430, set = 0, binding = 0) readonly buffer Views {
    CameraCull views[];
};

// The groups in drawing order (CullDrawOrder::order).
layout(std430, set = 0, binding = 1) readonly buffer Order {
    uint drawOrder[];
};

layout(std430, set = 0, binding = 2) readonly buffer Groups {
    CullGroup groups[];
};

// Each group's camera command, unpacked (from the view's command slice).
layout(std430, set = 0, binding = 3) readonly buffer Commands {
    DrawCommand commands[];
};

// Each place's end in the work (the pairs before it and its own), setup.x places.
layout(std430, set = 0, binding = 4) buffer WorkEnds {
    uint workEnds[];
};

// MESH_CLUSTER_WORK_WORDS: the styles' first pairs, their end, the real pairs.
layout(std430, set = 0, binding = 5) buffer Work {
    uint work[];
};

// The frame's draw list, instances, records and clusters.
layout(std430, set = 0, binding = 6) readonly buffer DrawList {
    uint drawList[];
};

layout(std430, set = 0, binding = 7) readonly buffer Instances {
    InstanceData instances[];
};

layout(std430, set = 0, binding = 8) readonly buffer Records {
    CullRecord records[];
};

layout(std430, set = 0, binding = 9) readonly buffer Clusters {
    MeshCluster clusters[];
};

// x: places in the drawing order's pooled runs, y: room for block sums, z: room for pairs.
layout(std430, set = 0, binding = 10) readonly buffer Setup {
    uvec4 setup;
};

// What occlusion is tested with (GpuMeshClusterOcclusion): last frame's camera-relative view-projection taking this
// frame's camera-relative points, this frame's (jittered, as the depth was drawn), this frame's camera, and the pyramid
// (width, height, levels, MESH_CLUSTER_OCCLUSION_* flags).
layout(std430, set = 0, binding = 11) readonly buffer Occlusion {
    mat4 previous;
    mat4 current;
    vec4 origin;
    uvec4 pyramid;
} occlusion;

// The list this dispatch makes (MESH_CLUSTER_EARLY or MESH_CLUSTER_LATE): its totals, items and task commands.
layout(push_constant) uniform MeshClusterList {
    uint list;
} pass;

// A group's command.
DrawCommand groupCommand(uint group) {
    return commands[views[0].slices.z + group];
}

// A group's pairs: its drawn instances times its clusters.
uint groupPairs(uint group) {
    return groupCommand(group).instanceCount * groups[group].clusterCount;
}

// The first of the list's totals.
uint listTotals() {
    return pass.list * uint(MESH_CLUSTER_TOTAL_WORDS);
}

#ifdef MESH_CLUSTER_PYRAMID_BINDING
// Last frame's depth pyramid (farthest depths) and this frame's, from the early list's depth: exact texels.
layout(set = 0, binding = MESH_CLUSTER_PYRAMID_BINDING) uniform sampler2D lastPyramid;
layout(set = 0, binding = MESH_CLUSTER_PYRAMID_BINDING + 1) uniform sampler2D thisPyramid;

/*
* Whether a pyramid hides a sphere (measured from the camera) seen through the view-projection: every cell its box covers,
* at the level where it spans at most two each way, farther than its nearest point (meshClusterHidden). The two pyramids
* are of one size, so the occlusion record's.
*/
bool meshClusterHiddenIn(sampler2D pyramid, vec4 sphere, mat4 viewProjection) {
    ivec2 size = ivec2(occlusion.pyramid.xy);
    MeshClusterScreenBox box = meshClusterScreenBox(sphere, viewProjection, vec2(size));

    if (!box.onScreen) {
        return false;
    }

    ivec4 pixels = meshClusterBoxPixels(box.box, size);

    if (pixels.x > pixels.z || pixels.y > pixels.w) {
        return false;
    }

    int level = meshClusterPyramidLevel(pixels, int(occlusion.pyramid.z));
    ivec2 levelSize = textureSize(pyramid, level);
    ivec4 cells = ivec4(pyramidCell(pixels.x, level, levelSize.x), pyramidCell(pixels.y, level, levelSize.y), pyramidCell(pixels.z, level, levelSize.x),
                        pyramidCell(pixels.w, level, levelSize.y));
    float farthest = GPU_NEAR_DEPTH;

    for (int y = cells.y; y <= cells.w; ++y) {
        for (int x = cells.x; x <= cells.z; ++x) {
            farthest = fartherDepth(farthest, texelFetch(pyramid, ivec2(x, y), level).r);
        }
    }

    return box.nearestDepth < farthest;
}

/*
* The pair at a place in the work: the first place whose end is past it (a binary search), none in a style's padding or
* past the work; then whether this list keeps it (cullMeshClustersOnCpu's choosePair): hidden in neither; seen whole, or
* occlusion off, in the early list only; else the early list keeps what last frame's pyramid does not hide (all, with no
* history), the late list what it hid (a candidate) that this frame's does not.
*/
bool meshClusterPairKept(uint pair, out MeshClusterItem item, out bool candidate) {
    item = MeshClusterItem(0u, 0u);
    candidate = false;
    uint places = setup.x;
    uint low = 0u;
    uint high = places;

    while (low < high) {
        uint middle = (low + high) / 2u;

        if (workEnds[middle] > pair) {
            high = middle;
        } else {
            low = middle + 1u;
        }
    }

    if (low >= places) {
        return false;
    }

    uint group = drawOrder[low];
    uint first = workEnds[low] - groupPairs(group);

    if (pair < first) {
        return false;
    }

    uint within = pair - first;
    CullGroup culled = groups[group];
    item = MeshClusterItem(groupCommand(group).firstInstance + within / culled.clusterCount, culled.firstCluster + within % culled.clusterCount);
    uint entry = drawList[item.entry];
    uint draw = entry & DRAW_LIST_INSTANCE_MASK;
    MeshCluster cluster = clusters[item.cluster];
    InstanceData instance = instances[draw];
    uint visibility = meshClusterVisibility(cluster, instance, entry, records[draw], views[0]);
    bool late = pass.list == MESH_CLUSTER_LATE;

    if (visibility == MESH_CLUSTER_HIDDEN) {
        return false;
    }

    if (visibility == MESH_CLUSTER_SEEN || (occlusion.pyramid.w & MESH_CLUSTER_OCCLUSION_ON) == 0u) {
        return !late;
    }

    vec4 world = meshClusterWorldSphere(cluster, instance.model);
    vec4 sphere = vec4(world.xyz - occlusion.origin.xyz, world.w);
    bool hiddenBefore = (occlusion.pyramid.w & MESH_CLUSTER_OCCLUSION_HISTORY) != 0u && meshClusterHiddenIn(lastPyramid, sphere, occlusion.previous);

    if (!late) {
        return !hiddenBefore;
    }

    candidate = hiddenBefore;
    return hiddenBefore && !meshClusterHiddenIn(thisPyramid, sphere, occlusion.current);
}
#endif

#endif
