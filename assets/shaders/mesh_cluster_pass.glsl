// What the mesh cluster passes share (render/vulkan/vulkan_mesh_cluster_culling.h): the tables they read and the pair at
// a place in the work (mirrors meshClusterPairAt and cullMeshClustersOnCpu's test, render/gpu/mesh_cluster_culling.h).
// The passes that test pairs define MESH_CLUSTER_PYRAMID_BINDING (after their storage buffers) for the two pyramids.
#ifndef MESH_CLUSTER_PASS_GLSL
#define MESH_CLUSTER_PASS_GLSL

#include "mesh_cluster_culling.glsl"
#include "placed_drawing.glsl"
#include "twig_drawing.glsl"

// The bones (bone_cull_data.glsl): each plant's tree, the trees, the pool, the twigs' bindings.
#define BONE_CULL_BINDING MESH_CLUSTER_BONE_BINDING
#include "bone_cull_data.glsl"

// The camera's view as its culling placed it, with the wind's sway (its own copy: the camera culls without it).
layout(std430, set = 0, binding = 0) readonly buffer Views {
    CameraCull views[];
};

// The groups in drawing order (meshClusterDrawOrder: each style's draw groups, then its placed groups past setup.w).
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

// x: places in the drawing order, y: room for block sums, z: room for pairs, w: the draw groups (a place's group past
// them is a placed group's).
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

// The camera's placed plants (render/gpu/placed_drawing.h; one unused record each without): its commands by placed
// group (the camera is the placed drawing's first view), the placed groups, every view's entries, each part's instance
// and record in its plant's frame, and the slots' plants.
layout(std430, set = 0, binding = 12) readonly buffer PlacedCommands {
    DrawCommand placedCommands[];
};

layout(std430, set = 0, binding = 13) readonly buffer PlacedGroups {
    PlacedGroup placedGroups[];
};

layout(std430, set = 0, binding = 14) readonly buffer PlacedEntries {
    uvec2 placedEntries[];
};

layout(std430, set = 0, binding = 15) readonly buffer PlacedPartInstances {
    InstanceData placedPartInstances[];
};

layout(std430, set = 0, binding = 16) readonly buffer PlacedParts {
    PlacedPart placedParts[];
};

layout(std430, set = 0, binding = 17) readonly buffer PlacedPlants {
    PlacedPlant placedPlants[];
};

// The camera's twigs (render/gpu/twig_records.h; one unused record each without): each region's entries the twig pass
// wrote (region r's draw r + 1 clusters each) and a region's room, the entries (a placed entry's place, the twig, its
// level's first cluster) and the twigs.
layout(std430, set = 0, binding = 18) readonly buffer TwigRegions {
    uvec4 twigCounts;
    uvec4 twigRoom; // x
};

layout(std430, set = 0, binding = 19) readonly buffer TwigEntries {
    uvec4 twigEntries[];
};

layout(std430, set = 0, binding = 20) readonly buffer Twigs {
    Twig twigs[];
};

// The list this dispatch makes (MESH_CLUSTER_EARLY or MESH_CLUSTER_LATE): its totals, items and task commands; and the
// frame's bones stamp (GpuWindField::flags[1]: 0 when the bones pass did not run).
layout(push_constant) uniform MeshClusterList {
    uint list;
    uint boneStamp;
} pass;

// A group's command: a draw group's, past them a placed group's, from TWIG_GROUP_BASE a twig region's: its entries
// written (at most its room) from its first place (orderedGroup).
DrawCommand groupCommand(uint group) {
    if (group >= TWIG_GROUP_BASE) {
        uint region = group - TWIG_GROUP_BASE;
        DrawCommand command;
        command.indexCount = 0u;
        command.instanceCount = min(twigCounts[region], twigRoom.x);
        command.firstIndex = 0u;
        command.vertexOffset = 0;
        command.firstInstance = TWIG_INSTANCE_BASE + region * twigRoom.x;
        return command;
    }

    return group < setup.w ? commands[views[0].slices.z + group] : placedCommands[group - setup.w];
}

// A group's clusters: first, count (a twig region's first is each entry's own: meshClusterPairKept).
uvec2 groupClusters(uint group) {
    if (group >= TWIG_GROUP_BASE) {
        return uvec2(0u, group - TWIG_GROUP_BASE + 1u);
    }

    if (group < setup.w) {
        return uvec2(groups[group].firstCluster, groups[group].clusterCount);
    }

    PlacedGroup placed = placedGroups[group - setup.w];
    return uvec2(placed.firstCluster, placed.clusterCount);
}

// A group's pairs: its drawn instances times its clusters.
uint groupPairs(uint group) {
    return groupCommand(group).instanceCount * groupClusters(group).y;
}

// What a pair's place draws (meshClusterDrawnAt): its instance, entry bits and record; a placed plant's number
// (BONE_NO_TREE for a frame's draw), and a twig's part and number (MESH_CLUSTER_NO_TWIG otherwise).
struct MeshClusterDrawn {
    InstanceData instance;
    uint entry;
    CullRecord record;
    uint plant;
    InstanceData part;
    uint twig;
};

/*
* A frame's draw through the draw list, or, from PLACED_INSTANCE_BASE, a placed entry's part placed by its plant, as the
* vertex shaders' drawnObject does. Mirrors meshClusterDrawnAt.
*/
MeshClusterDrawn meshClusterDrawnAt(uint place) {
    MeshClusterDrawn drawn;

    // A twig: its part placed by its plant, then the twig after it (twigInstance).
    bool twig = place >= TWIG_INSTANCE_BASE;
    uvec4 twigEntry = twig ? twigEntries[place - TWIG_INSTANCE_BASE] : uvec4(0u);
    place = twig ? PLACED_INSTANCE_BASE + twigEntry.x : place;

    if (place >= PLACED_INSTANCE_BASE) {
        uvec2 placed = placedEntries[place - PLACED_INSTANCE_BASE];
        drawn.plant = placed.x & DRAW_LIST_INSTANCE_MASK;
        PlacedPlant plant = placedPlants[drawn.plant];
        drawn.instance = placedInstance(placedPartInstances[placed.y], plant);
        drawn.record = placedCullRecord(placedParts[placed.y].record, plant);
        drawn.entry = placed.x;
        drawn.part = drawn.instance;
        drawn.twig = MESH_CLUSTER_NO_TWIG;

        if (twig) {
            drawn.instance = twigInstance(drawn.part, twigs[twigEntry.y]);
            drawn.twig = twigEntry.y;
        }

        return drawn;
    }

    drawn.entry = drawList[place];
    uint draw = drawn.entry & DRAW_LIST_INSTANCE_MASK;
    drawn.instance = instances[draw];
    drawn.record = records[draw];
    drawn.plant = BONE_NO_TREE;
    drawn.part = drawn.instance;
    drawn.twig = MESH_CLUSTER_NO_TWIG;
    return drawn;
}

/*
* How a pair's cluster stands (meshClusterPoseAt): a twig's by its binding and tree, on its cluster's own sphere; a placed
* plant's bound cluster by its tree's bones, padded by the flutter; a frame's draw's bound cluster stands still.
*/
MeshClusterPose meshClusterPoseAt(MeshClusterDrawn drawn, MeshCluster cluster) {
    BoneTree tree = drawn.plant == BONE_NO_TREE ? BoneTree(0u, 0u, BONE_NO_POSE, 0.0, vec4(0.0)) : boneCullTree(drawn.plant, pass.boneStamp);

    if (drawn.twig != MESH_CLUSTER_NO_TWIG) {
        uint binding = drawn.twig < uint(boneCullTwigBindings.length()) ? boneCullTwigBindings[drawn.twig] : SWAY_UNBOUND;
        return twigPose(cluster.sphere, drawn.part, twigs[drawn.twig], binding, tree);
    }

    if (!meshClusterBound(cluster)) {
        return meshClusterUnposed(false);
    }

    if (tree.firstPose == BONE_NO_POSE) {
        return meshClusterUnposed(true);
    }

    return meshClusterPosed(meshClusterBonesSphere(cluster, tree), drawn.instance.model, drawn.instance.sway.w, tree.leans.xy);
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
    uvec2 grouped = groupClusters(group);
    uint entry = groupCommand(group).firstInstance + within / grouped.y;
    uint firstCluster = group >= TWIG_GROUP_BASE ? twigEntries[entry - TWIG_INSTANCE_BASE].z : grouped.x;
    item = MeshClusterItem(entry, firstCluster + within % grouped.y);
    MeshClusterDrawn drawn = meshClusterDrawnAt(item.entry);
    MeshCluster cluster = clusters[item.cluster];
    InstanceData instance = drawn.instance;
    MeshClusterPose pose = meshClusterPoseAt(drawn, cluster);
    uint visibility = meshClusterVisibility(cluster, instance, drawn.entry, drawn.record, views[0], pose);
    bool late = pass.list == MESH_CLUSTER_LATE;

    if (visibility == MESH_CLUSTER_HIDDEN) {
        return false;
    }

    if (visibility == MESH_CLUSTER_SEEN || (occlusion.pyramid.w & MESH_CLUSTER_OCCLUSION_ON) == 0u) {
        return !late;
    }

    vec4 world = meshClusterCullSphere(cluster, instance.model, pose);
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
