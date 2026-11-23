// The bones as the camera's culling reads them (ROADMAP phase 21 stage 3c-iv-b; render/gpu/bone_poses.h, BoneCullTables),
// included by the passes that cull trees by their posed bounds after defining BONE_CULL_BINDING, the first of its four
// storage buffers in set 0: each held plant's word pair (stamp, tree), the trees, the pool and each twig's binding. Its
// readers mirror boneCullTree, meshClusterBonesSphere and twigPose. The draws read the same records through the scene
// set instead (bone_vertex_data.glsl).
#ifndef BONE_CULL_DATA_GLSL
#define BONE_CULL_DATA_GLSL

#ifndef BONE_CULL_BINDING
#error "bone_cull_data.glsl needs BONE_CULL_BINDING"
#endif

#include "twig_drawing.glsl"

layout(std430, set = 0, binding = BONE_CULL_BINDING) readonly buffer BoneCullPlantTrees {
    uint boneCullPlantTrees[];
};

layout(std430, set = 0, binding = BONE_CULL_BINDING + 1) readonly buffer BoneCullTrees {
    BoneTree boneCullTrees[];
};

layout(std430, set = 0, binding = BONE_CULL_BINDING + 2) readonly buffer BoneCullPoses {
    BonePose boneCullPoses[];
};

// A word a twig (packTwigBinding), in the twig table's order.
layout(std430, set = 0, binding = BONE_CULL_BINDING + 3) readonly buffer BoneCullTwigBindings {
    uint boneCullTwigBindings[];
};

/*
* The tree posing a plant this frame (boneCullTree): claimed with this frame's stamp (0: the bones pass did not run,
* nothing matches) and given room, else one with firstPose BONE_NO_POSE.
*/
BoneTree boneCullTree(uint plant, uint stamp) {
    BoneTree none = BoneTree(0u, 0u, BONE_NO_POSE, 0.0, vec4(0.0));

    if (plant >= uint(boneCullPlantTrees.length()) / 2u) {
        return none;
    }

    uint tree = boneClaimedTree(boneCullPlantTrees[2u * plant], boneCullPlantTrees[2u * plant + 1u], stamp);
    return tree == BONE_NO_TREE || tree >= uint(boneCullTrees.length()) ? none : boneCullTrees[tree];
}

WindBonePose boneCullPoseAt(uint slot) {
    BonePose pose = boneCullPoses[slot];
    return WindBonePose(pose.turn, pose.shift.xyz);
}

// A binding's two poses this frame from its tree's first pose (boneVertexPoses): its bone's, the one its pose names as parent.
BoneVertexPoses boneCullVertexPoses(uint firstPose, uint binding) {
    uint own = bonePoseSlot(firstPose, binding & 0xffffu, false);
    int parent = floatBitsToInt(boneCullPoses[own].shift.w);
    WindBonePose parentPose = parent < 0 ? windRestPose() : boneCullPoseAt(bonePoseSlot(firstPose, uint(parent), false));
    return BoneVertexPoses(boneCullPoseAt(own), parentPose, float(binding >> 16u) / 65535.0);
}

// A cluster's bone in a slot: two to a word, the first four in bones, the rest in moreBones (meshClusterBone).
uint meshClusterBoneAt(MeshCluster cluster, uint slot) {
    uint word = slot / 2u;
    uint packed = word < 2u ? cluster.bones[word] : cluster.moreBones[word - 2u];
    return (packed >> (16u * (slot % 2u))) & 0xFFFFu;
}

// A bound cluster's sphere in its mesh's frame as its posed tree's bones carry it (meshClusterBonesSphere).
vec4 meshClusterBonesSphere(MeshCluster cluster, BoneTree tree) {
    if (meshClusterBoneAt(cluster, 0u) == MESH_CLUSTER_MANY_BONES) {
        return vec4(cluster.sphere.xyz, cluster.sphere.w + tree.move);
    }

    vec4 posed = cluster.sphere;

    for (uint slot = 0u; slot < MESH_CLUSTER_MOST_BONES; ++slot) {
        uint bone = meshClusterBoneAt(cluster, slot);

        if (bone == MESH_CLUSTER_NO_BONE) {
            break;
        }

        BoneVertexPoses poses = boneCullVertexPoses(tree.firstPose, bone);
        vec4 blended = windBlendedSphere(poses.own, poses.parent, cluster.sphere);
        posed = slot == 0u ? blended : windSphereUnion(posed, blended);
    }

    return posed;
}

// How a twig stands (twigPose): bound by its binding; posed with its tree, its sphere (its prototype's frame) placed by
// the twig carried by its rigid pose at its foot, padded by the flutter.
MeshClusterPose twigPose(vec4 sphere, InstanceData part, Twig twig, uint binding, BoneTree tree) {
    bool bound = binding != SWAY_UNBOUND;

    if (!bound || tree.firstPose == BONE_NO_POSE) {
        return meshClusterUnposed(bound);
    }

    BoneVertexPoses poses = boneCullVertexPoses(tree.firstPose, binding);
    WindBonePose rigid = windRigidPose(poses.own, poses.parent, poses.weight, vec3(twig.rows[0].w, twig.rows[1].w, twig.rows[2].w));
    return meshClusterPosed(sphere, twigInstance(part, twigPosed(twig, rigid)).model, part.sway.w, tree.leans.xy);
}

#endif
