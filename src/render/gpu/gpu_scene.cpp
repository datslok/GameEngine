#include "render/gpu/gpu_scene.h"
#include "core/hash.h"
#include "render/gpu/camera_relative.h"
#include "render/gpu/column_major.h"
#include "render/gpu/shadow_map.h"

#include <algorithm>
#include <cstring>

/*
* The same products in the same order as the per-draw uniforms had, so every vertex lands exactly where it did.
*/
GpuInstance packInstance(const DrawItem& draw, const Mat4& relativeViewProjection, const Vec3& cameraOrigin, std::uint32_t material) {
    const Mat4 relativeModel = relativeTo(draw.model, cameraOrigin);

    GpuInstance instance{};
    writeColumnMajor(relativeViewProjection * relativeModel, instance.transform);
    writeColumnMajor(relativeModel, instance.model);
    writeColumnMajor(normalMatrix(draw.model), instance.normalMatrix);
    instance.handedness[0] = handedness(draw.model);
    instance.material[0] = material;
    return instance;
}

GpuShadowInstance packShadowInstance(const Mat4& tileMatrix, const Mat4& model) {
    GpuShadowInstance instance{};
    writeColumnMajor(tileMatrix * model, instance.transform);
    return instance;
}

// The atlas never changes size, so its texel is a constant; it rides along here because the shader wants it per pixel.
GpuFrameData makeFrameData(const GpuLights& lights) {
    GpuFrameData frame{};
    std::copy(lights.ambient, lights.ambient + 4, frame.ambient);
    std::copy(lights.counts, lights.counts + 4, frame.counts);
    frame.atlasTexel[0] = 1.0f / static_cast<float>(shadowAtlasWidth);
    frame.atlasTexel[1] = 1.0f / static_cast<float>(shadowAtlasHeight);
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
