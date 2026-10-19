#include "render/gpu/gpu_scene.h"
#include "core/hash.h"
#include "render/gpu/column_major.h"
#include "render/gpu/shadow_map.h"

#include <algorithm>
#include <cstring>

/*
* World space, so the shadow pass (whose cache keys on world matrices) and the camera share the record; the camera's
* subtraction happens in the vertex shader, on the matrix, before any product (see camera_relative.h).
*/
GpuInstance packInstance(const DrawItem& draw, std::uint32_t material) {
    GpuInstance instance{};
    writeColumnMajor(draw.model, instance.model);
    writeColumnMajor(normalMatrix(draw.model), instance.normalMatrix);
    instance.handedness[0] = handedness(draw.model);
    instance.material[0] = material;
    return instance;
}

// Column-major like every matrix the shaders read, for vkCmdPushConstants.
GpuShadowView packShadowView(const Mat4& tileMatrix) {
    GpuShadowView view{};
    writeColumnMajor(tileMatrix, view.matrix);
    return view;
}

// The atlas never changes size, so its texel is a constant; it rides along here because the shader wants it per pixel.
namespace {
    void writeAxis(const Vec3& axis, float destination[4]) {
        destination[0] = axis.x;
        destination[1] = axis.y;
        destination[2] = axis.z;
    }
}

GpuFrameData makeFrameData(const GpuLights& lights, const Mat4& relativeViewProjection, const Vec3& cameraOrigin,
                           const ClusterGrid& clusters, bool clusteredShading, bool clusterHeatmap) {
    GpuFrameData frame{};
    writeColumnMajor(relativeViewProjection, frame.relativeViewProjection);
    frame.cameraOrigin[0] = cameraOrigin.x;
    frame.cameraOrigin[1] = cameraOrigin.y;
    frame.cameraOrigin[2] = cameraOrigin.z;
    std::copy(lights.ambient, lights.ambient + 4, frame.ambient);
    std::copy(lights.counts, lights.counts + 4, frame.counts);
    frame.atlasTexel[0] = 1.0f / static_cast<float>(shadowAtlasWidth);
    frame.atlasTexel[1] = 1.0f / static_cast<float>(shadowAtlasHeight);
    frame.clusterTiles[0] = clusters.tilesX;
    frame.clusterTiles[1] = clusters.tilesY;
    frame.clusterTiles[2] = clusters.slices;
    frame.clusterScreen[0] = static_cast<float>(clusters.screenWidth);
    frame.clusterScreen[1] = static_cast<float>(clusters.screenHeight);
    frame.clusterScreen[2] = clusters.tanHalfWidth;
    frame.clusterScreen[3] = clusters.tanHalfHeight;
    frame.clusterSlicing[0] = clusters.sliceScale;
    frame.clusterSlicing[1] = clusters.sliceBias;
    writeAxis(clusters.right, frame.viewRight);
    writeAxis(clusters.up, frame.viewUp);
    writeAxis(clusters.forward, frame.viewForward);
    frame.shadingOptions[0] = clusteredShading ? 1 : 0;
    frame.shadingOptions[1] = clusterHeatmap ? 1 : 0;
    return frame;
}

/*
* GpuMaterial is all 4-byte fields with no padding (its size is checked), so its bytes can be hashed and compared as one
* block. Two materials pack the same exactly when they shade the same.
*/
std::uint32_t MaterialTable::add(const Material& material) {
    const GpuMaterial packed = packMaterial(material);

    Fnv1aHash hash;
    hash.addBytes(&packed, sizeof(packed));

    const auto [first, last] = byHash.equal_range(hash.value);

    for (auto found = first; found != last; ++found) {
        if (std::memcmp(&entries[found->second], &packed, sizeof(packed)) == 0) {
            return found->second;
        }
    }

    const std::uint32_t index = static_cast<std::uint32_t>(entries.size());
    entries.push_back(packed);
    byHash.emplace(hash.value, index);
    return index;
}

const std::vector<GpuMaterial>& MaterialTable::getEntries() const {
    return entries;
}

/*
* A stable sort by mesh keeps each mesh's draws in the order they came, then each run of one mesh becomes a batch.
*/
BatchedDraws batchByMesh(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen, std::uint32_t firstInstance) {
    BatchedDraws batched;
    batched.order = chosen;

    std::stable_sort(batched.order.begin(), batched.order.end(), [&draws](std::size_t a, std::size_t b) {
        return draws[a].mesh.index < draws[b].mesh.index;
    });

    for (std::size_t position = 0; position < batched.order.size(); ++position) {
        const MeshHandle mesh = draws[batched.order[position]].mesh;

        if (batched.batches.empty() || batched.batches.back().mesh != mesh) {
            batched.batches.push_back(DrawBatch{mesh, firstInstance + static_cast<std::uint32_t>(position), 0});
        }

        ++batched.batches.back().instanceCount;
    }

    return batched;
}
