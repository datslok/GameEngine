// The mesh shaders drawing the camera's mesh clusters (triangle.mesh; the main pass's, with its motion vectors,
// triangle_motion.mesh): one workgroup a surviving (draw list entry, cluster) pair of the style pushed
// (render/gpu/mesh_cluster_culling.h), its vertices placed by the vertex shaders' own body (triangle_vertex.glsl,
// shadeVertex) and its triangles from the cluster's corner bytes. Included after the extensions it needs.
#define MESH_CLUSTER_SHADER

#include "render/gpu/mesh_cluster_culling_shared.h"
#include "scene/mesh_clusters_shared.h"

layout(local_size_x = MESH_CLUSTER_MESH_THREADS) in;
layout(triangles, max_vertices = MESH_CLUSTER_MAX_VERTICES, max_primitives = MESH_CLUSTER_MAX_TRIANGLES) out;

// The depth prepass and the main pass both draw a cluster with these shaders, and the main pass shades only where the
// depth EQUALS the prepass's: invariant makes both compute the position alike, bit for bit (as the vertex shaders do).
out gl_MeshPerVertexEXT {
    invariant vec4 gl_Position;
    float gl_ClipDistance[1];
} gl_MeshVerticesEXT[];

#include "triangle_vertex.glsl"
#include "mesh_cluster_culling.glsl"
#include "mesh_cluster_pyramid.glsl"

// The style drawn, its list (MESH_CLUSTER_EARLY or _LATE) and the pyramid its far leaves are tested against
// (MESH_CLUSTER_LEAF_TEST_*) (render/vulkan/scene_layout.h: after the vertex shaders' push constants).
layout(push_constant) uniform MeshClusterDraw {
    layout(offset = MESH_CLUSTER_PUSH_OFFSET) uint drawnStyle;
    uint drawnList;
    uint farLeafTest;
};

// The occlusion pyramids (render/vulkan/vulkan_mesh_cluster_culling.h): this frame's early list's depth, and the whole
// frame's (built after the prepass, the next frame's history).
layout(set = 1, binding = SCENE_LEAF_PYRAMID_EARLY_BINDING) uniform sampler2D earlyLeafPyramid;
layout(set = 1, binding = SCENE_LEAF_PYRAMID_WHOLE_BINDING) uniform sampler2D wholeLeafPyramid;

// Whether a primitive is culled before rasterising: a far leaf hidden behind the pyramid; every other false.
out perprimitiveEXT gl_MeshPerPrimitiveEXT {
    bool gl_CullPrimitiveEXT;
} gl_MeshPrimitivesEXT[];

// The buffers, by the addresses frame_data.glsl's meshClusterBuffers holds.
layout(buffer_reference, std430) readonly buffer MeshClusterItems {
    MeshClusterItem items[];
};

layout(buffer_reference, std430) readonly buffer MeshClusterRecords {
    MeshCluster records[];
};

layout(buffer_reference, std430) readonly buffer MeshClusterWords {
    uint words[];
};

// The vertex pool: MeshVertex, twelve floats (position, normal, uv, tangent).
layout(buffer_reference, std430) readonly buffer MeshVertexFloats {
    float values[];
};

// A twig's pose, made by the workgroup's first thread for all its vertices (one twig a pair).
shared TwigPose workgroupTwig;

// A far leaf cluster's leaves (stage 3e-iii-b): as many as fit a cluster's vertices and triangles, each its own twig's
// pose, made once a leaf for its five vertices.
const uint FAR_LEAVES_A_CLUSTER = min(uint(MESH_CLUSTER_MAX_VERTICES) / uint(FAR_LEAF_VERTICES), uint(MESH_CLUSTER_MAX_TRIANGLES) / uint(FAR_LEAF_TRIANGLES));
shared TwigPose farLeafPoses[FAR_LEAVES_A_CLUSTER];

// Each far leaf vertex's clip position as placed, whether each leaf is hidden, and how many are (stage 3e-v-b).
shared vec4 farLeafClips[FAR_LEAVES_A_CLUSTER * uint(FAR_LEAF_VERTICES)];
shared uint farLeafHidden[FAR_LEAVES_A_CLUSTER];
shared uint farLeavesHidden;

// The totals buffer written: the far leaf tally after both lists' totals (MESH_CLUSTER_LEAF_TALLY_FIRST).
layout(buffer_reference, std430) buffer MeshClusterTally {
    uint words[];
};

/*
* Whether a far leaf is hidden behind this draw's pyramid: its vertices' box (farLeafScreenBox) wholly behind it. The
* pyramid is the render size, as the viewport its clip positions land in.
*/
bool farLeafBehindPyramid(uint leaf) {
    vec4 clips[FAR_LEAF_VERTICES];

    for (uint vertex = 0u; vertex < uint(FAR_LEAF_VERTICES); ++vertex) {
        clips[vertex] = farLeafClips[leaf * uint(FAR_LEAF_VERTICES) + vertex];
    }

    if (farLeafTest == MESH_CLUSTER_LEAF_TEST_EARLY) {
        return meshClusterBoxHiddenIn(earlyLeafPyramid, farLeafScreenBox(clips, vec2(textureSize(earlyLeafPyramid, 0))));
    }

    return meshClusterBoxHiddenIn(wholeLeafPyramid, farLeafScreenBox(clips, vec2(textureSize(wholeLeafPyramid, 0))));
}

/*
* A far leaf cluster's pair (its cluster's number with TWIG_FAR_CLUSTER_BIT): each leaf's record made into its row
* (farLeafVertex, in the plant's frame), placed as its part's vertex by the vertex shaders' body, riding its own twig's
* pose and taking its twig's tone and lean (farLeafPose); its triangles each leaf's four. With a pyramid pushed, a leaf
* hidden behind it has its triangles culled before rasterising: a leaf is far smaller than its cluster, so hides far
* more often (one test a leaf, at the triangles that cost). Its vertices are still placed: their count is set first.
*/
void drawFarCluster(MeshClusterItem item) {
    uint far = item.cluster & ~TWIG_FAR_CLUSTER_BIT;
    uvec4 run = sceneFarClusters[2u * far + 1u];
    uint leaves = min(run.y, FAR_LEAVES_A_CLUSTER);
    uint vertexCount = leaves * uint(FAR_LEAF_VERTICES);
    uint triangleCount = leaves * uint(FAR_LEAF_TRIANGLES);
    SetMeshOutputsEXT(vertexCount, triangleCount);
    uint plantEntry = scenePlacedEntries[sceneTwigEntries[item.entry - TWIG_INSTANCE_BASE].x].x;
    uint plantPick = floatBitsToUint(scenePlacedPlants[drawnInstance(plantEntry)].pick);
    uint fade = drawListFade(plantEntry);

    for (uint leaf = gl_LocalInvocationIndex; leaf < leaves; leaf += MESH_CLUSTER_MESH_THREADS) {
        farLeafPoses[leaf] = farLeafPose(plantEntry, sceneFarLeaves[run.x + leaf].twig);
    }

    barrier();

    for (uint slot = gl_LocalInvocationIndex; slot < vertexCount; slot += MESH_CLUSTER_MESH_THREADS) {
        uint leaf = slot / uint(FAR_LEAF_VERTICES);
        FarLeaf record = sceneFarLeaves[run.x + leaf];
        FarLeafShape shape = sceneFarShapes[run.z + ((record.sideCellNumber >> 16) & 0xffu)];
        FarLeafVertex made = farLeafVertex(record, shape, slot % uint(FAR_LEAF_VERTICES));

        // A crown fading into its card: the leaf shrinks about its foot at its own twig's rank, so it goes with its twig.
        if (fade != 0u) {
            made.position = thinnedPosition(made.position, vec3(record.foot[0], record.foot[1], record.foot[2]),
                                            twigFadePresence(fade, twigFadeRank(plantPick, record.twig)));
        }

        vec3 relative = shadeVertex(slot, made.position, made.normal, made.uv, made.tangent, made.position, item.entry, item.entry, 0, 0u, farLeafPoses[leaf]);
        farLeafClips[slot] = relativeViewProjection * vec4(relative, 1.0);
    }

    // The push constant is the same for the whole draw, so every thread meets these barriers.
    bool testing = farLeafTest != MESH_CLUSTER_LEAF_TEST_NONE;

    if (testing) {
        if (gl_LocalInvocationIndex == 0u) {
            farLeavesHidden = 0u;
        }

        barrier();

        for (uint leaf = gl_LocalInvocationIndex; leaf < leaves; leaf += MESH_CLUSTER_MESH_THREADS) {
            bool hidden = farLeafBehindPyramid(leaf);
            farLeafHidden[leaf] = hidden ? 1u : 0u;

            if (hidden) {
                atomicAdd(farLeavesHidden, 1u);
            }
        }

        barrier();

        // The tally (F4's FAR line): the main pass's words first, then the late prepass's; once a workgroup.
        if (gl_LocalInvocationIndex == 0u) {
            MeshClusterTally tally = MeshClusterTally(meshClusterBuffers[0].zw);
            uint first = uint(MESH_CLUSTER_LEAF_TALLY_FIRST) + (farLeafTest == MESH_CLUSTER_LEAF_TEST_WHOLE ? 0u : 2u);
            atomicAdd(tally.words[first], leaves);
            atomicAdd(tally.words[first + 1u], farLeavesHidden);
        }
    }

    for (uint triangle = gl_LocalInvocationIndex; triangle < triangleCount; triangle += MESH_CLUSTER_MESH_THREADS) {
        uint leaf = triangle / uint(FAR_LEAF_TRIANGLES);
        gl_PrimitiveTriangleIndicesEXT[triangle] = farLeafTriangle(triangle % uint(FAR_LEAF_TRIANGLES)) + uvec3(leaf * uint(FAR_LEAF_VERTICES));
        gl_MeshPrimitivesEXT[triangle].gl_CullPrimitiveEXT = testing && farLeafHidden[leaf] != 0u;
    }
}

void main() {
    // The workgroup's pair within its style's run of its list, the same for the whole workgroup; none past the run.
    MeshClusterWords totals = MeshClusterWords(meshClusterBuffers[0].zw);
    uint listTotals = drawnList * uint(MESH_CLUSTER_TOTAL_WORDS);
    uint inRun = meshClusterTaskIndex(gl_WorkGroupID);

    if (inRun >= totals.words[listTotals + MESH_CLUSTER_TOTAL_RUN_COUNT + drawnStyle]) {
        SetMeshOutputsEXT(0u, 0u);
        return;
    }

    uint place = totals.words[listTotals + MESH_CLUSTER_TOTAL_LIST_FIRST] + totals.words[listTotals + MESH_CLUSTER_TOTAL_RUN_FIRST + drawnStyle] + inRun;
    MeshClusterItem item = MeshClusterItems(meshClusterBuffers[0].xy).items[place];

    // The same for the whole workgroup, so its barriers are met by every thread.
    if ((item.cluster & TWIG_FAR_CLUSTER_BIT) != 0u) {
        drawFarCluster(item);
        return;
    }
    MeshCluster cluster = MeshClusterRecords(meshClusterBuffers[1].xy).records[item.cluster];
    uint vertexCount = cluster.counts & 0xffu;
    uint triangleCount = cluster.counts >> MESH_CLUSTER_TRIANGLE_SHIFT;
    SetMeshOutputsEXT(vertexCount, triangleCount);

    MeshClusterWords vertexNumbers = MeshClusterWords(meshClusterBuffers[1].zw);
    MeshClusterWords triangleWords = MeshClusterWords(meshClusterBuffers[2].xy);
    MeshVertexFloats vertices = MeshVertexFloats(meshClusterBuffers[2].zw);

    if (gl_LocalInvocationIndex == 0u) {
        workgroupTwig = twigPoseOf(item.entry);
    }

    barrier();
    TwigPose twigPose = workgroupTwig;

    // Each vertex placed as the vertex shaders place it; a pooled mesh never deforms, so last frame's shape is its own.
    // The pair's place stands for the draw's first instance too, so a placed plant's (from PLACED_INSTANCE_BASE) is
    // built from its plant and part (drawnObject), as an indexed draw of it would be.
    for (uint slot = gl_LocalInvocationIndex; slot < vertexCount; slot += MESH_CLUSTER_MESH_THREADS) {
        uint number = vertexNumbers.words[cluster.firstVertex + slot];
        uint at = number * 12u;
        vec3 position = vec3(vertices.values[at], vertices.values[at + 1u], vertices.values[at + 2u]);
        vec3 normal = vec3(vertices.values[at + 3u], vertices.values[at + 4u], vertices.values[at + 5u]);
        vec2 uv = vec2(vertices.values[at + 6u], vertices.values[at + 7u]);
        vec4 tangent = vec4(vertices.values[at + 8u], vertices.values[at + 9u], vertices.values[at + 10u], vertices.values[at + 11u]);
        shadeVertex(slot, position, normal, uv, tangent, position, item.entry, item.entry, int(number - cluster.meshFirstVertex), number, twigPose);
    }

    // Each triangle's three corner bytes, four to a word.
    for (uint triangle = gl_LocalInvocationIndex; triangle < triangleCount; triangle += MESH_CLUSTER_MESH_THREADS) {
        uvec3 corners;

        for (uint corner = 0u; corner < 3u; ++corner) {
            uint byte = triangle * 3u + corner;
            corners[corner] = (triangleWords.words[cluster.firstTriangleWord + byte / 4u] >> (8u * (byte % 4u))) & 0xffu;
        }

        gl_PrimitiveTriangleIndicesEXT[triangle] = corners;
        gl_MeshPrimitivesEXT[triangle].gl_CullPrimitiveEXT = false;
    }
}
