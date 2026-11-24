// Mesh clusters culled a view, included with #include "mesh_cluster_culling.glsl": mirrors render/gpu/mesh_cluster_culling.h
// (meshClusterLargestScale, meshClusterPlacedSphere, meshClusterWorldSphere, meshClusterErrorPixels, meshClusterInCut,
// meshClusterEyeInMesh, meshClusterFacesAway, meshClusterShapeFixed, meshClusterSways, meshClusterPosed, meshClusterSeenWhole,
// meshClusterBound, meshClusterCullSphere, meshClusterVisibility, meshClusterKept, meshClusterScreenBox, meshClusterEmptyBox,
// meshClusterBoxTake, farLeafScreenBox, meshClusterBoxPixels, meshClusterPyramidLevel). Pure functions: the pyramid's and
// the bones' reads are the passes' (mesh_cluster_pyramid.glsl, meshClusterBoxHidden; bone_cull_data.glsl).
#ifndef MESH_CLUSTER_CULLING_GLSL
#define MESH_CLUSTER_CULLING_GLSL

#include "render/gpu/instance_shared.h"
#include "render/gpu/mesh_cluster_culling_shared.h"
#include "scene/twig_far_levels_shared.h"
#include "camera_culling.glsl"
#include "instance_data.glsl"

// pyramidCell, fartherDepth, ndcToPixel.
#include "screen_reflections.glsl"

// windFlutterReach.
#include "wind.glsl"

// A cluster of a mesh in the pools (scene/mesh_clusters.h). Must match GpuMeshCluster.
struct MeshCluster {
    vec4 sphere;           // xyz centre, w radius, in the mesh's space
    vec4 cone;             // xyz apex, w cutoff (1 or more: never culled by facing)
    vec3 axis;
    uint firstVertex;      // its vertices' run in the cluster vertex pool (each the vertex's number in the vertex pool)
    uint firstTriangleWord; // its triangles' run in the triangle pool: three corner bytes each, four to a word
    uint counts;           // vertices, then triangles from MESH_CLUSTER_TRIANGLE_SHIFT
    uint meshFirstVertex;  // its mesh's first vertex in the vertex pool
    float ownError;        // the cut, in the mesh's space: the group it was made from (0 for the mesh's own triangles)
    vec4 ownSphere;        // and its sphere
    vec4 parentSphere;     // the group it was simplified with: its sphere
    float parentError;     // and the error of what was made from it (above MESH_CLUSTER_NEVER_COARSER: nothing was)
    uint level;            // the level of the group it was simplified with (scene/mesh_clusters.h)
    uvec2 bones;           // up to MESH_CLUSTER_MOST_BONES of its vertices' bones, 16 bits each (meshClusterBones): the first four
    uvec2 moreBones;       // and the last four (two uvec2, not a uvec4, which would align past level)
    uvec2 unused;
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

// A sphere in the mesh's space placed in the world: its centre moved, its radius by the longest a unit can stretch
// (meshClusterPlacedSphere).
vec4 meshClusterPlacedSphere(vec4 sphere, mat4 model) {
    return vec4((model * vec4(sphere.xyz, 1.0)).xyz, sphere.w * meshClusterLargestScale(model));
}

// The cluster's sphere in the world (meshClusterWorldSphere).
vec4 meshClusterWorldSphere(MeshCluster cluster, mat4 model) {
    return meshClusterPlacedSphere(cluster.sphere, model);
}

/*
* How many pixels a group's error (mesh units) covers: stretched by the model's longest stretch, over the distance to
* its placed sphere (at least MESH_CLUSTER_NEAREST_ERROR_DISTANCE), times the view's pixels a metre away; distance-free
* in an orthographic view (meshClusterErrorPixels). precise: a cluster's own error and its children's parent error are
* the same group's, so both sides of the choice must round alike, without fused products.
*/
float meshClusterErrorPixels(vec4 sphere, float error, mat4 model, LevelView levels) {
    precise float pixels = error * meshClusterLargestScale(model) * levels.eye.w;

    if ((levels.flags & CULL_LEVELS_ORTHOGRAPHIC) != 0u) {
        return pixels;
    }

    precise vec4 placed = meshClusterPlacedSphere(sphere, model);
    precise vec3 offset = placed.xyz - levels.eye.xyz;
    precise float distance = max(sqrt(dot(offset, offset)) - placed.w, MESH_CLUSTER_NEAREST_ERROR_DISTANCE);
    precise float result = pixels / distance;
    return result;
}

// Whether the cut draws the cluster: its own error fits the view's pixel error and its parent's does not, or nothing
// coarser was made (meshClusterInCut).
bool meshClusterInCut(MeshCluster cluster, mat4 model, LevelView levels) {
    if (meshClusterErrorPixels(cluster.ownSphere, cluster.ownError, model, levels) > levels.pixelError) {
        return false;
    }

    return cluster.parentError > MESH_CLUSTER_NEVER_COARSER ||
           meshClusterErrorPixels(cluster.parentSphere, cluster.parentError, model, levels) > levels.pixelError;
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
* Whether the draw's shape is its mesh's, moved only by its matrix and its bones (meshClusterShapeFixed): not a terrain
* patch, an impostor's card, ground-following, a liana, or cards thinned in this view.
*/
bool meshClusterShapeFixed(InstanceData instance, uint entry) {
    return instance.material.z == 0u && instance.material.w == uint(INSTANCE_KIND_PLAIN) && instance.hanging.w == 0u &&
           (entry & DRAW_LIST_THINNED_BIT) == 0u;
}

// Whether the draw sways per vertex where the view's sway reaches, as cameraCandidateFlags tests it (meshClusterSways).
bool meshClusterSways(CullRecord record, CameraCull view) {
    return (view.flags & CAMERA_CULL_SWAY) != 0u && (record.flags & CULL_SWAYS) != 0u && swayReaches(record.bounds, view.levels.eye.xyz, view.swayFadeEnd);
}

// How a cluster stands this frame (MeshClusterPose): bound to bones, posed, and then its sphere in the world.
struct MeshClusterPose {
    bool bound;
    bool posed;
    vec4 sphere;
};

MeshClusterPose meshClusterUnposed(bool bound) {
    return MeshClusterPose(bound, false, vec4(0.0));
}

// A posed thing's pose: its posed sphere placed by the model, grown by its leaves' flutter's reach (meshClusterPosed).
MeshClusterPose meshClusterPosed(vec4 sphere, mat4 placedBy, float flutter, vec2 lean) {
    vec4 placed = meshClusterPlacedSphere(sphere, placedBy);
    placed.w += windFlutterReach(flutter, lean);
    return MeshClusterPose(true, true, placed);
}

// Whether the view keeps every cluster of the draw (meshClusterSeenWhole): its shape not its mesh's, or swaying unbound.
bool meshClusterSeenWhole(InstanceData instance, uint entry, CullRecord record, CameraCull view, MeshClusterPose pose) {
    return !meshClusterShapeFixed(instance, entry) || (!pose.bound && meshClusterSways(record, view));
}

// Whether a cluster's vertices are bound to bones: its first slot not empty (meshClusterBound).
bool meshClusterBound(MeshCluster cluster) {
    return (cluster.bones.x & 0xFFFFu) != MESH_CLUSTER_NO_BONE;
}

// The sphere the view tests a cluster by: its pose's when posed, else its own placed (meshClusterCullSphere).
vec4 meshClusterCullSphere(MeshCluster cluster, mat4 model, MeshClusterPose pose) {
    return pose.posed ? pose.sphere : meshClusterWorldSphere(cluster, model);
}

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
* What the view makes of a cluster (meshClusterVisibility: MESH_CLUSTER_HIDDEN, _SEEN, _SPHERE): hidden out of the cut
* (any draw); seen whole (meshClusterSeenWhole); else hidden out of the frustum (meshClusterCullSphere) or, single-sided,
* not mirrored and not posed, facing away; else seen, its sphere holding.
*/
uint meshClusterVisibility(MeshCluster cluster, InstanceData instance, uint entry, CullRecord record, CameraCull view, MeshClusterPose pose) {
    if (!meshClusterInCut(cluster, instance.model, view.levels)) {
        return MESH_CLUSTER_HIDDEN;
    }

    if (meshClusterSeenWhole(instance, entry, record, view, pose)) {
        return MESH_CLUSTER_SEEN;
    }

    if (!sphereInFrustum(view.frustum, meshClusterCullSphere(cluster, instance.model, pose))) {
        return MESH_CLUSTER_HIDDEN;
    }

    float determinant = meshClusterDeterminant(instance.model);

    if (pose.posed || (record.flags & CULL_DOUBLE_SIDED) != 0u || !(determinant > 0.0)) {
        return MESH_CLUSTER_SPHERE;
    }

    return meshClusterFacesAway(cluster, meshClusterEyeInMesh(instance.model, view.levels.eye.xyz, determinant)) ? MESH_CLUSTER_HIDDEN : MESH_CLUSTER_SPHERE;
}

bool meshClusterKept(MeshCluster cluster, InstanceData instance, uint entry, CullRecord record, CameraCull view, MeshClusterPose pose) {
    return meshClusterVisibility(cluster, instance, entry, record, view, pose) != MESH_CLUSTER_HIDDEN;
}

// A sphere's box on screen (meshClusterScreenBox): the pixels it covers, least then most, and its nearest depth.
struct MeshClusterScreenBox {
    vec4 box;
    float nearestDepth;
    bool onScreen;
};

// A box holding nothing yet, least past most (meshClusterEmptyBox).
MeshClusterScreenBox meshClusterEmptyBox() {
    return MeshClusterScreenBox(vec4(3.4e38, 3.4e38, -3.4e38, -3.4e38), -3.4e38, false);
}

// The box grown to hold a clip-space point; false, the box untouched, when it is within MESH_CLUSTER_SMALLEST_W of the
// camera's plane or behind it (meshClusterBoxTake).
bool meshClusterBoxTake(inout MeshClusterScreenBox box, vec4 clip, vec2 size) {
    if (!(clip.w > MESH_CLUSTER_SMALLEST_W)) {
        return false;
    }

    vec2 pixel = ndcToPixel(clip.xy / clip.w, size);
    box.box.xy = min(box.box.xy, pixel);
    box.box.zw = max(box.box.zw, pixel);
    box.nearestDepth = max(box.nearestDepth, clip.z / clip.w);
    return true;
}

/*
* A sphere (xyz centre measured from the camera, w radius) through a camera-relative view-projection: its box's eight
* corners' pixels, least and most, and the nearest corner depth; not on screen when a corner is within
* MESH_CLUSTER_SMALLEST_W of the camera's plane or behind it (meshClusterScreenBox).
*/
MeshClusterScreenBox meshClusterScreenBox(vec4 sphere, mat4 viewProjection, vec2 size) {
    MeshClusterScreenBox result = meshClusterEmptyBox();

    for (int corner = 0; corner < 8; ++corner) {
        vec3 offset = vec3((corner & 1) != 0 ? sphere.w : -sphere.w, (corner & 2) != 0 ? sphere.w : -sphere.w, (corner & 4) != 0 ? sphere.w : -sphere.w);

        if (!meshClusterBoxTake(result, viewProjection * vec4(sphere.xyz + offset, 1.0), size)) {
            return MeshClusterScreenBox(vec4(0.0), 0.0, false);
        }
    }

    result.onScreen = true;
    return result;
}

/*
* A far leaf's screen box from its vertices' clip positions (farLeafScreenBox, stage 3e-v-b): their box, their nearest
* depth taken MESH_CLUSTER_LEAF_DEPTH_MARGIN nearer (the main pass's pyramid holds the leaf's own depth, rounded apart).
*/
MeshClusterScreenBox farLeafScreenBox(vec4 clips[FAR_LEAF_VERTICES], vec2 size) {
    MeshClusterScreenBox result = meshClusterEmptyBox();

    for (int vertex = 0; vertex < FAR_LEAF_VERTICES; ++vertex) {
        if (!meshClusterBoxTake(result, clips[vertex], size)) {
            return MeshClusterScreenBox(vec4(0.0), 0.0, false);
        }
    }

    result.nearestDepth *= 1.0 + MESH_CLUSTER_LEAF_DEPTH_MARGIN;
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
