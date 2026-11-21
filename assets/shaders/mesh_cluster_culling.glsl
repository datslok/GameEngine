// Mesh clusters culled a view, included with #include "mesh_cluster_culling.glsl": mirrors render/gpu/mesh_cluster_culling.h
// (meshClusterLargestScale, meshClusterWorldSphere, meshClusterEyeInMesh, meshClusterFacesAway, meshClusterRigid,
// meshClusterVisibility, meshClusterKept, meshClusterScreenBox, meshClusterBoxPixels, meshClusterPyramidLevel). Pure
// functions: the pyramid's reads are the passes' (mesh_cluster_pass.glsl, meshClusterHidden).
#ifndef MESH_CLUSTER_CULLING_GLSL
#define MESH_CLUSTER_CULLING_GLSL

#include "render/gpu/instance_shared.h"
#include "render/gpu/mesh_cluster_culling_shared.h"
#include "camera_culling.glsl"
#include "instance_data.glsl"

// pyramidCell, fartherDepth, ndcToPixel.
#include "screen_reflections.glsl"

// A cluster of a mesh in the pools (scene/mesh_clusters.h). Must match GpuMeshCluster.
struct MeshCluster {
    vec4 sphere;           // xyz centre, w radius, in the mesh's space
    vec4 cone;             // xyz apex, w cutoff (1 or more: never culled by facing)
    vec3 axis;
    uint firstVertex;      // its vertices' run in the cluster vertex pool (each the vertex's number in the vertex pool)
    uint firstTriangleWord; // its triangles' run in the triangle pool: three corner bytes each, four to a word
    uint counts;           // vertices, then triangles from MESH_CLUSTER_TRIANGLE_SHIFT
    uint meshFirstVertex;  // its mesh's first vertex in the vertex pool
    uint unused;
};

// A (draw list entry, cluster) pair a list draws. Must match GpuMeshClusterItem.
struct MeshClusterItem {
    uint entry;   // the place in the draw list (the instance index the vertex shaders read)
    uint cluster; // the cluster's number in the pool
};

// The longest a unit vector can come out of the matrix's turning part: its longest column (meshClusterLargestScale).
float meshClusterLargestScale(mat4 model) {
    vec3 x = model[0].xyz;
    vec3 y = model[1].xyz;
    vec3 z = model[2].xyz;
    return sqrt(max(max(dot(x, x), dot(y, y)), dot(z, z)));
}

// The cluster's sphere in the world: its centre placed, its radius by the longest a unit can stretch (meshClusterWorldSphere).
vec4 meshClusterWorldSphere(MeshCluster cluster, mat4 model) {
    return vec4((model * vec4(cluster.sphere.xyz, 1.0)).xyz, cluster.sphere.w * meshClusterLargestScale(model));
}

/*
* A point in the world brought into the mesh's space: the turning part's inverse (its columns' cross products over its
* determinant, which the caller has checked is positive) applied to the point less the translation (meshClusterEyeInMesh).
*/
vec3 meshClusterEyeInMesh(mat4 model, vec3 eye, float determinant) {
    vec3 x = model[0].xyz;
    vec3 y = model[1].xyz;
    vec3 z = model[2].xyz;
    vec3 offset = eye - model[3].xyz;
    return vec3(dot(cross(y, z), offset), dot(cross(z, x), offset), dot(cross(x, y), offset)) / determinant;
}

// The turning part's determinant: below 0 mirrors, 0 flattens.
float meshClusterDeterminant(mat4 model) {
    return dot(model[0].xyz, cross(model[1].xyz, model[2].xyz));
}

// Every triangle's back faces the eye (in the mesh's space): the eye to the apex within the cutoff of the axis
// (scene/mesh_clusters.h, clusterFacesAway).
bool meshClusterFacesAway(MeshCluster cluster, vec3 eye) {
    if (cluster.cone.w >= 1.0) {
        return false;
    }

    vec3 toApex = cluster.cone.xyz - eye;
    float distance = sqrt(dot(toApex, toApex));
    return distance > 0.0 && dot(toApex, cluster.axis) >= cluster.cone.w * distance;
}

/*
* Whether the draw's vertices are where its mesh has them, moved only by its matrix, so its clusters' spheres and cones
* hold (meshClusterRigid): not a terrain patch, an impostor's card, ground-following, a liana, cards thinned in this
* view, or swaying where the view's sway reaches (as cameraCandidateFlags tests it).
*/
bool meshClusterRigid(InstanceData instance, uint entry, CullRecord record, CameraCull view) {
    if (instance.material.z != 0u || instance.material.w != uint(INSTANCE_KIND_PLAIN) || instance.hanging.w != 0u) {
        return false;
    }

    if ((entry & DRAW_LIST_THINNED_BIT) != 0u) {
        return false;
    }

    return !((view.flags & CAMERA_CULL_SWAY) != 0u && (record.flags & CULL_SWAYS) != 0u &&
             swayReaches(record.bounds, view.levels.eye.xyz, view.swayFadeEnd));
}

/*
* Whether the view draws a cluster of a drawn instance (meshClusterKept): every cluster of a draw that is not rigid;
* else its sphere in the frustum, and, single-sided and not mirrored, not facing away from the eye.
*/
/*
* A style's survivors as mesh shader workgroups (meshClusterTaskGroups): a row of up to MESH_CLUSTER_TASK_WIDTH, then as
* many rows as it takes; none for none.
*/
uvec3 meshClusterTaskGroups(uint count) {
    return count == 0u ? uvec3(0u, 1u, 1u) : uvec3(min(count, MESH_CLUSTER_TASK_WIDTH), (count + MESH_CLUSTER_TASK_WIDTH - 1u) / MESH_CLUSTER_TASK_WIDTH, 1u);
}

// A workgroup's survivor within its style's run (meshClusterTaskIndex): rows of MESH_CLUSTER_TASK_WIDTH; past the run's
// count in the last row, none.
uint meshClusterTaskIndex(uvec3 group) {
    return group.y * MESH_CLUSTER_TASK_WIDTH + group.x;
}

/*
* What the view makes of a cluster (meshClusterVisibility: MESH_CLUSTER_HIDDEN, _SEEN, _SPHERE): seen whole for a draw
* not rigid; else hidden out of the frustum or, single-sided and not mirrored, facing away; else seen, its sphere holding.
*/
uint meshClusterVisibility(MeshCluster cluster, InstanceData instance, uint entry, CullRecord record, CameraCull view) {
    if (!meshClusterRigid(instance, entry, record, view)) {
        return MESH_CLUSTER_SEEN;
    }

    if (!sphereInFrustum(view.frustum, meshClusterWorldSphere(cluster, instance.model))) {
        return MESH_CLUSTER_HIDDEN;
    }

    float determinant = meshClusterDeterminant(instance.model);

    if ((record.flags & CULL_DOUBLE_SIDED) != 0u || !(determinant > 0.0)) {
        return MESH_CLUSTER_SPHERE;
    }

    return meshClusterFacesAway(cluster, meshClusterEyeInMesh(instance.model, view.levels.eye.xyz, determinant)) ? MESH_CLUSTER_HIDDEN : MESH_CLUSTER_SPHERE;
}

bool meshClusterKept(MeshCluster cluster, InstanceData instance, uint entry, CullRecord record, CameraCull view) {
    return meshClusterVisibility(cluster, instance, entry, record, view) != MESH_CLUSTER_HIDDEN;
}

// A sphere's box on screen (meshClusterScreenBox): the pixels it covers, least then most, and its nearest depth.
struct MeshClusterScreenBox {
    vec4 box;
    float nearestDepth;
    bool onScreen;
};

/*
* A sphere (xyz centre measured from the camera, w radius) through a camera-relative view-projection: its box's eight
* corners' pixels, least and most, and the nearest corner depth; not on screen when a corner is within
* MESH_CLUSTER_SMALLEST_W of the camera's plane or behind it (meshClusterScreenBox).
*/
MeshClusterScreenBox meshClusterScreenBox(vec4 sphere, mat4 viewProjection, vec2 size) {
    MeshClusterScreenBox result = MeshClusterScreenBox(vec4(3.4e38, 3.4e38, -3.4e38, -3.4e38), -3.4e38, false);

    for (int corner = 0; corner < 8; ++corner) {
        vec3 offset = vec3((corner & 1) != 0 ? sphere.w : -sphere.w, (corner & 2) != 0 ? sphere.w : -sphere.w, (corner & 4) != 0 ? sphere.w : -sphere.w);
        vec4 clip = viewProjection * vec4(sphere.xyz + offset, 1.0);

        if (!(clip.w > MESH_CLUSTER_SMALLEST_W)) {
            return MeshClusterScreenBox(vec4(0.0), 0.0, false);
        }

        vec2 pixel = ndcToPixel(clip.xy / clip.w, size);
        result.box.xy = min(result.box.xy, pixel);
        result.box.zw = max(result.box.zw, pixel);
        result.nearestDepth = max(result.nearestDepth, clip.z / clip.w);
    }

    result.onScreen = true;
    return result;
}

// The pixels a box covers, clamped to the screen; empty (first past last) off screen (meshClusterBoxPixels).
ivec4 meshClusterBoxPixels(vec4 box, ivec2 size) {
    vec4 limits = vec4(size, size);
    ivec4 whole = ivec4(floor(clamp(box, vec4(-1.0), limits)));
    return ivec4(max(whole.xy, ivec2(0)), min(whole.zw, size - 1));
}

// The level where the pixels span at most two cells each way, within the levels (meshClusterPyramidLevel).
int meshClusterPyramidLevel(ivec4 pixels, int levels) {
    int extent = max(pixels.z - pixels.x, pixels.w - pixels.y);
    int level = extent <= 1 ? 0 : findMSB(extent - 1) + 1;
    return min(level, levels - 1);
}

#endif
