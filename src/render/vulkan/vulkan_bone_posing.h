#pragma once

#include "math/vec3.h"
#include "render/gpu/bone_poses.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

class VulkanDevice;

/*
* The trees' bones posed on the GPU (ROADMAP phase 21 stage 3c-iii-b; render/gpu/bone_poses.h), after the wind's springs
* are stepped and before the twigs' and mesh clusters' passes: bones_claim (a workgroup a group of a part moved by bones
* and swaying view: each plant drawn claimed once by its frame stamp, a tree and its bones' room in the pool) and
* bones_pose (indirect, a workgroup a tree: its lean, then its chains level by level). The poses are for the vertex and
* mesh shaders (3c-iii-c) and the clusters' bounds (3c-iv).
*
* The skeleton tables (bones, skeletons, chains, levels' starts) are made when the placed table's skeletons change and
* shared by every frame in flight. The header, trees, plant table and pool are shared too: each frame's passes wait for
* the frames before to finish reading them (a barrier from every earlier use: frames on one queue run in order anyway).
* The work list and the read-back of the counts (and, for bones-compare, the trees and pool) are each frame's own.
*/
class VulkanBonePosing {
public:
    // textures, scene: the sets the pose pass reads the wind's record and springs through (set 1 the scene's).
    VulkanBonePosing(const VulkanDevice& device, VkDescriptorSetLayout textures, VkDescriptorSetLayout scene, std::uint32_t framesInFlight);

    // Between frames only: anything not yet collected is dropped.
    void setFramesInFlight(std::uint32_t count);

    /*
    * The frame's tables from the placed table (after the placed drawing's prepare): none (returns false) without
    * skeletons. plantedParts: the frame's, as the table's draws index them (each skeleton's lean part). plants: the
    * slots' plants' room (a plant table word pair each). Remade when the skeletons or their lean parts change, the
    * pool when its size (the setting's, or grown by collect) or the plants' room does, the GPU idle first. Throws std::invalid_argument as packSwaySkeletons.
    */
    bool prepare(const PlacedTable& table, std::span<const DrawItem> plantedParts, std::uint32_t plants, const WindBoneMotion& motion,
                 std::uint32_t frameIndex);

    // What the passes read of the frame's placed plants, and the frame's swaying views (placed view numbers).
    struct Inputs {
        VkBuffer commands = VK_NULL_HANDLE; // every view's
        VkBuffer entries = VK_NULL_HANDLE;
        VkBuffer parts = VK_NULL_HANDLE;
        VkBuffer plants = VK_NULL_HANDLE;
        VkDescriptorBufferInfo partInstances{};
        std::uint32_t groupsAView = 0;
        std::vector<std::uint32_t> swayingViews;
        VkDescriptorSet textures = VK_NULL_HANDLE;
        VkDescriptorSet scene = VK_NULL_HANDLE;
        Vec3 eye;
        float fadeEnd = 0.0f;
        bool readBackPoses = false; // for bones-compare: the trees and pool copied back too
    };

    // Record the passes (after the wind's step and the placed passes, outside any rendering block). Nothing without swaying views.
    void record(VkCommandBuffer commands, FrameDescriptors& descriptors, const Inputs& inputs, std::uint32_t frameIndex);

    // What a frame counted: trees claimed, still for want of room, bones asked, the pool's room; with readBackPoses, the
    // trees (claimed, up to BONE_MOST_TREES) and the pool as written.
    struct Collected {
        std::uint32_t trees = 0;
        std::uint32_t still = 0;
        std::uint32_t bonesAsked = 0;
        std::uint32_t pool = 0;
        std::vector<GpuBoneTree> posedTrees;
        std::vector<GpuBonePose> poses;
    };

    // After a frame's fence: what it counted (none when it posed nothing); an ask past the pool grows it (boneGrownPool).
    std::optional<Collected> collect(std::uint32_t frameIndex);

    // The tables the frame's passes read (after prepare), for bones-compare.
    const SwaySkeletonTables& getTables() const;

    // The stamp the next record claims with, for the draws (GpuWindField::flags[1]) written before it.
    std::uint32_t getNextStamp() const;

    // For the draws reading the poses (3c-iii-c): the trees, each plant's tree, the pool (VK_NULL_HANDLE before any).
    VkBuffer getTrees() const;
    VkBuffer getPlantTrees() const;
    VkBuffer getPoses() const;

private:
    struct PerFrame {
        VulkanBuffer work;
        VulkanBuffer readback;
        VulkanBuffer treesReadback;
        VulkanBuffer posesReadback;
        bool pending = false;
        bool posesPending = false;
    };

    const VulkanDevice& device;
    std::unique_ptr<ComputePipeline> claimPass;
    std::unique_ptr<ComputePipeline> posePass;
    std::vector<PerFrame> frames;

    // The skeleton tables and what they were made from.
    SwaySkeletonTables tables;
    std::vector<const SwaySkeleton*> madeFrom;
    std::vector<std::uint32_t> madeLeanParts;
    VulkanBuffer bones;
    VulkanBuffer skeletons;
    VulkanBuffer chains;
    VulkanBuffer levelStarts;
    std::vector<std::uint32_t> groups;

    // Shared by every frame: the header, trees, each plant's tree (cleared when made), the pool.
    VulkanBuffer header;
    VulkanBuffer trees;
    VulkanBuffer plantTrees;
    VulkanBuffer poses;
    bool plantTreesCleared = false;
    std::uint32_t poolBones = 0;

    // The pool's settings it started from, and the size frames have grown it to (boneGrownPool), made at the next prepare.
    int poolSetting = 0;
    int poolMostSetting = 0;
    std::uint32_t grownBones = 0;
    std::uint32_t stamp = 0;
};
