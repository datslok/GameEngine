// Twigs drawn as instances of prototypes, included with #include "twig_drawing.glsl": mirrors render/gpu/twig_records.h
// (twigInstance, twigPosed, twigLevel, twigBrickKept, twigKept, twigBrickPose, twigSpherePose, twigFarLevelFits, twigFarNodeKept, twigFarMeshCluster) and declares its records. Pure functions, the errors' sides precise,
// so the twig pass chooses what the cluster cut would.
#ifndef TWIG_DRAWING_GLSL
#define TWIG_DRAWING_GLSL

#include "bone_poses.glsl"
#include "mesh_cluster_culling.glsl"
#include "twig_record.glsl"

// A brick: its sphere in the plant's frame and its twigs' run. Must match GpuTwigBrick.
struct TwigBrick {
    vec4 sphere;
    uint firstTwig;
    uint twigCount;
    uint unused0;
    uint unused1;
};

// A prototype's level: its group's sphere and error, its clusters' run and triangles. Must match GpuTwigLevel.
struct TwigLevel {
    vec4 sphere;
    float error;
    uint firstCluster;
    uint clusterCount;
    uint triangleCount;
};

// A prototype's levels in the level table. Must match GpuTwigPrototype.
struct TwigPrototype {
    uint firstLevel;
    uint levelCount;
    uint unused0;
    uint unused1;
};

// A twig of a placed part (twigInstance): the twig's matrix after the part's, its turn after the part's normals, its
// scale (its across column's length) on the part's.
InstanceData twigInstance(InstanceData part, Twig twig) {
    mat4 matrix = twigMatrix(twig);
    mat4 turn = matrix;
    turn[3] = vec4(0.0, 0.0, 0.0, 1.0);
    InstanceData instance = part;
    instance.model = part.model * matrix;
    instance.normalMatrix = part.normalMatrix * turn;
    instance.handedness.y = part.handedness.y * length(vec3(twig.rows[0].x, twig.rows[1].x, twig.rows[2].x));
    return instance;
}

// A twig carried whole by a rigid pose in the plant's frame: the pose after its matrix (twigPosed).
Twig twigPosed(Twig twig, WindBonePose pose) {
    Twig posed = twig;

    for (int column = 0; column < 4; ++column) {
        vec3 turned = quatRotate(pose.turn, vec3(twig.rows[0][column], twig.rows[1][column], twig.rows[2][column])) + (column == 3 ? pose.shift : vec3(0.0));
        posed.rows[0][column] = turned.x;
        posed.rows[1][column] = turned.y;
        posed.rows[2][column] = turned.z;
    }

    return posed;
}

/*
* Whether a twig draws a level (twigLevelDrawn; the pass walks a prototype's levels with it, as twigLevel does): its own
* error (the level before's group's, none for the finest) fits the view and its group's does not.
*/
bool twigLevelDrawn(TwigLevel before, bool finest, TwigLevel level, mat4 model, LevelView view) {
    bool ownFits = finest || meshClusterErrorPixels(before.sphere, before.error, model, view) <= view.pixelError;
    bool coarserFits = level.error <= MESH_CLUSTER_NEVER_COARSER && meshClusterErrorPixels(level.sphere, level.error, model, view) <= view.pixelError;
    return ownFits && !coarserFits;
}

// Whether a view keeps a sphere placed by a model: always where the part is seen whole, else its posed sphere or its
// own placed one in the frustum.
bool twigSphereKept(vec4 sphere, mat4 placedBy, InstanceData part, uint entry, CullRecord record, CameraCull view, MeshClusterPose pose) {
    if (meshClusterSeenWhole(part, entry, record, view, pose)) {
        return true;
    }

    return sphereInFrustum(view.frustum, pose.posed ? pose.sphere : meshClusterPlacedSphere(sphere, placedBy));
}

// A brick (twigBrickKept): its sphere in the plant's frame, placed by the part.
bool twigBrickKept(TwigBrick brick, InstanceData part, uint entry, CullRecord record, CameraCull view, MeshClusterPose pose) {
    return twigSphereKept(brick.sphere, part.model, part, entry, record, view, pose);
}

// A twig (twigKept): its prototype's coarsest group's sphere, placed by the twig.
bool twigKept(TwigLevel coarsest, InstanceData twig, uint entry, CullRecord record, CameraCull view, MeshClusterPose pose) {
    return twigSphereKept(coarsest.sphere, twig.model, twig, entry, record, view, pose);
}

// A far node: its sphere in the plant's frame, its levels' run, its first brick, its parent. Must match GpuTwigFarNode.
struct TwigFarNode {
    vec4 sphere;
    uint firstLevel;
    uint levelCount;
    uint firstBrick;
    uint parent;
};

// A far node's level: its clusters' run, its error and its leaves. Must match GpuTwigFarLevel.
struct TwigFarLevel {
    uint firstCluster;
    uint clusterCount;
    float error;
    uint leafCount;
};

// A far leaf cluster: the sphere of its leaves' vertices, their run, its set's first cell shape. Must match GpuTwigFarCluster.
struct TwigFarCluster {
    vec4 sphere;
    uint firstLeaf;
    uint leafCount;
    uint firstShape;
    uint unused;
};

// A far leaf cluster as the cluster passes test it (twigFarMeshCluster): its sphere as its own and its group's, its
// leaves' vertices and triangles, always in the cut, never culled by facing, bound to no bone.
MeshCluster twigFarMeshCluster(TwigFarCluster far) {
    MeshCluster cluster;
    cluster.sphere = far.sphere;
    cluster.cone = vec4(0.0, 0.0, 0.0, 1.0);
    cluster.axis = vec3(0.0);
    cluster.firstVertex = 0u;
    cluster.firstTriangleWord = 0u;
    cluster.counts = far.leafCount * uint(FAR_LEAF_VERTICES) | (far.leafCount * uint(FAR_LEAF_TRIANGLES)) << MESH_CLUSTER_TRIANGLE_SHIFT;
    cluster.meshFirstVertex = 0u;
    cluster.ownError = 0.0;
    cluster.ownSphere = far.sphere;
    cluster.parentSphere = far.sphere;
    cluster.parentError = uintBitsToFloat(0x7f7fffffu); // the largest float: nothing coarser
    cluster.level = 0u;
    cluster.bones = uvec2(0xFFFFFFFFu);
    cluster.moreBones = uvec2(0xFFFFFFFFu);
    cluster.unused = uvec2(0u);
    return cluster;
}

// Whether a far node's level fits the view (twigFarLevelFits): its error on the node's sphere placed by the part's model.
bool twigFarLevelFits(TwigFarNode node, TwigFarLevel level, mat4 model, LevelView view) {
    return meshClusterErrorPixels(node.sphere, level.error, model, view) <= view.pixelError;
}

// Whether a view keeps a far node (twigFarNodeKept): its sphere in the plant's frame, placed by the part.
bool twigFarNodeKept(TwigFarNode node, InstanceData part, uint entry, CullRecord record, CameraCull view, MeshClusterPose pose) {
    return twigSphereKept(node.sphere, part.model, part, entry, record, view, pose);
}

// How a sphere of leaves carried by twigs stands (twigSpherePose: a brick's, a far node's, a far leaf cluster's): posed
// with its tree when its part is bound, grown by the tree's move, placed by the part, padded by the flutter.
MeshClusterPose twigSpherePose(vec4 sphere, InstanceData part, bool partBound, BoneTree tree) {
    if (!partBound || tree.firstPose == BONE_NO_POSE) {
        return meshClusterUnposed(partBound);
    }

    return meshClusterPosed(vec4(sphere.xyz, sphere.w + tree.move), part.model, part.sway.w, tree.leans.xy);
}

// How a brick stands (twigBrickPose): posed with its tree (tree.firstPose not BONE_NO_POSE) when its part is bound,
// its sphere grown by the tree's move, placed by the part, padded by the flutter.
MeshClusterPose twigBrickPose(TwigBrick brick, InstanceData part, bool partBound, BoneTree tree) {
    return twigSpherePose(brick.sphere, part, partBound, tree);
}

#endif
