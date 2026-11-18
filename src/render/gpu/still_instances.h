#pragma once

#include "core/job_system.h"
#include "render/gpu/gpu_scene.h"
#include "scene/frame_description.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

/*
* The still draws' GPU records kept from frame to frame (docs/research/PERSISTENTSCENE.md, stage 2): their instances, last-frame
* matrices (their own) and a material table holding their materials, packed once and repacked only where the frame
* built still draws again (FrameDescription::builtStillDraws). Packing every draw's 272-byte material and normal matrix
* every frame cost the forest's 35,000 objects about 10 ms of CPU.
*
* builtStillDraws is all that changed only when the frame follows the last one packed (stillPreviousVersion), so a frame
* described but never drawn, another count of still draws or a whole build packs them all; the same frame again packs
* nothing. Changes leave old entries in the table; past twice the entries of the last whole packing (plus a margin),
* everything is packed afresh. Render-only and API-free, so tests run it without a GPU.
*/
class StillInstances {
public:
    // Brings the records up to date with the frame on jobs and returns the ranges packed (for the renderer's checks).
    std::vector<DrawRange> update(const FrameDescription& frame, JobSystem& jobs);

    std::span<const GpuInstance> getInstances() const;
    std::span<const std::array<float, 16>> getPreviousModels() const;
    const MaterialTable& getMaterials() const;

    // The first material entry the last update added (0 when it packed the table afresh; the count when none).
    std::size_t getFirstNewMaterial() const;

private:
    void packAll(const FrameDescription& frame, JobSystem& jobs);
    void packRange(const FrameDescription& frame, const DrawRange& range, JobSystem& jobs);

    std::vector<GpuInstance> instances;
    std::vector<std::array<float, 16>> previousModels;
    MaterialTable materials;
    std::size_t entriesWhenWhole = 0;
    std::size_t firstNewMaterial = 0;
    std::uint64_t packedVersion = 0;
};

/*
* A frame's other records, packed now: the moving draws' (every draw after the still ones) instances and last-frame
* matrices, and a table over the still draws' (MaterialTable::over) holding the materials it lacks, numbered after its
* entries: the GPU holds the still table's entries, then these. The still table must outlive it.
*/
struct SceneRecords {
    std::vector<GpuInstance> moving;
    std::vector<std::array<float, 16>> movingPrevious;
    MaterialTable materials;
};

SceneRecords packMovingDraws(const FrameDescription& frame, const StillInstances& still, JobSystem& jobs);

// Each draw's last-frame matrix, column-major, or its model's when it has none (not moving).
std::vector<std::array<float, 16>> packPreviousModels(std::span<const DrawItem> draws, JobSystem& jobs);
