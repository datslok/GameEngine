// What the mesh cluster passes share (render/vulkan/vulkan_mesh_cluster_culling.h): the tables they read and the pair at
// a place in the work (mirrors meshClusterPairAt and cullMeshClustersOnCpu's test, render/gpu/mesh_cluster_culling.h).
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

// A group's command.
DrawCommand groupCommand(uint group) {
    return commands[views[0].slices.z + group];
}

// A group's pairs: its drawn instances times its clusters.
uint groupPairs(uint group) {
    return groupCommand(group).instanceCount * groups[group].clusterCount;
}

/*
* The pair at a place in the work: the first place whose end is past it (a binary search), none in a style's padding or
* past the work; then whether the camera keeps it (meshClusterKept).
*/
bool meshClusterPairKept(uint pair, out MeshClusterItem item) {
    item = MeshClusterItem(0u, 0u);
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
    return meshClusterKept(clusters[item.cluster], instances[draw], entry, records[draw], views[0]);
}

#endif
