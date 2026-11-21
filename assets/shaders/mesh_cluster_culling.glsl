// Mesh clusters culled a view, included with #include "mesh_cluster_culling.glsl": mirrors render/gpu/mesh_cluster_culling.h
// (meshClusterLargestScale, meshClusterWorldSphere, meshClusterEyeInMesh, meshClusterFacesAway, meshClusterRigid,
// meshClusterKept). Pure functions.
#ifndef MESH_CLUSTER_CULLING_GLSL
#define MESH_CLUSTER_CULLING_GLSL

#include "render/gpu/instance_shared.h"
#include "render/gpu/mesh_cluster_culling_shared.h"
#include "camera_culling.glsl"
#include "instance_data.glsl"

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
bool meshClusterKept(MeshCluster cluster, InstanceData instance, uint entry, CullRecord record, CameraCull view) {
    if (!meshClusterRigid(instance, entry, record, view)) {
        return true;
    }

    if (!sphereInFrustum(view.frustum, meshClusterWorldSphere(cluster, instance.model))) {
        return false;
    }

    float determinant = meshClusterDeterminant(instance.model);

    if ((record.flags & CULL_DOUBLE_SIDED) != 0u || !(determinant > 0.0)) {
        return true;
    }

    return !meshClusterFacesAway(cluster, meshClusterEyeInMesh(instance.model, view.levels.eye.xyz, determinant));
}

#endif
