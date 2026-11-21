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

// The style drawn (render/vulkan/scene_layout.h: after the vertex shaders' push constants).
layout(push_constant) uniform MeshClusterDraw {
    layout(offset = MESH_CLUSTER_PUSH_OFFSET) uint drawnStyle;
};

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

void main() {
    // The workgroup's pair within its style's run, the same for the whole workgroup; none past the run.
    MeshClusterWords totals = MeshClusterWords(meshClusterBuffers[0].zw);
    uint inRun = meshClusterTaskIndex(gl_WorkGroupID);

    if (inRun >= totals.words[MESH_CLUSTER_TOTAL_RUN_COUNT + drawnStyle]) {
        SetMeshOutputsEXT(0u, 0u);
        return;
    }

    MeshClusterItem item = MeshClusterItems(meshClusterBuffers[0].xy).items[totals.words[MESH_CLUSTER_TOTAL_RUN_FIRST + drawnStyle] + inRun];
    MeshCluster cluster = MeshClusterRecords(meshClusterBuffers[1].xy).records[item.cluster];
    uint vertexCount = cluster.counts & 0xffu;
    uint triangleCount = cluster.counts >> MESH_CLUSTER_TRIANGLE_SHIFT;
    SetMeshOutputsEXT(vertexCount, triangleCount);

    MeshClusterWords vertexNumbers = MeshClusterWords(meshClusterBuffers[1].zw);
    MeshClusterWords triangleWords = MeshClusterWords(meshClusterBuffers[2].xy);
    MeshVertexFloats vertices = MeshVertexFloats(meshClusterBuffers[2].zw);

    // Each vertex placed as the vertex shaders place it; a pooled mesh never deforms, so last frame's shape is its own.
    for (uint slot = gl_LocalInvocationIndex; slot < vertexCount; slot += MESH_CLUSTER_MESH_THREADS) {
        uint number = vertexNumbers.words[cluster.firstVertex + slot];
        uint at = number * 12u;
        vec3 position = vec3(vertices.values[at], vertices.values[at + 1u], vertices.values[at + 2u]);
        vec3 normal = vec3(vertices.values[at + 3u], vertices.values[at + 4u], vertices.values[at + 5u]);
        vec2 uv = vec2(vertices.values[at + 6u], vertices.values[at + 7u]);
        vec4 tangent = vec4(vertices.values[at + 8u], vertices.values[at + 9u], vertices.values[at + 10u], vertices.values[at + 11u]);
        shadeVertex(slot, position, normal, uv, tangent, position, item.entry, 0u, int(number - cluster.meshFirstVertex));
    }

    // Each triangle's three corner bytes, four to a word.
    for (uint triangle = gl_LocalInvocationIndex; triangle < triangleCount; triangle += MESH_CLUSTER_MESH_THREADS) {
        uvec3 corners;

        for (uint corner = 0u; corner < 3u; ++corner) {
            uint byte = triangle * 3u + corner;
            corners[corner] = (triangleWords.words[cluster.firstTriangleWord + byte / 4u] >> (8u * (byte % 4u))) & 0xffu;
        }

        gl_PrimitiveTriangleIndicesEXT[triangle] = corners;
    }
}
