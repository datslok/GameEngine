#include "render/gpu/gpu_scene.h"
#include "core/hash.h"
#include "render/gpu/column_major.h"
#include "render/gpu/shadow_map.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

/*
* World space, so the shadow pass (whose cache keys on world matrices) and the camera share the record; the camera's
* subtraction happens in the vertex shader, on the matrix, before any product (see camera_relative.h).
*/
GpuInstance packInstance(const DrawItem& draw, std::uint32_t material) {
    GpuInstance instance{};
    writeColumnMajor(draw.model, instance.model);
    writeColumnMajor(normalMatrix(draw.model), instance.normalMatrix);
    instance.handedness[0] = handedness(draw.model);
    instance.handedness[1] = uniformScale(draw.model);
    instance.material[0] = material;
    instance.material[1] = draw.mesh.index;
    instance.material[2] = draw.terrainPatch >= 0 ? static_cast<std::uint32_t>(draw.terrainPatch) + 1 : 0u;
    return instance;
}

// Column-major like every matrix the shaders read, for vkCmdPushConstants.
GpuShadowView packShadowView(const Mat4& tileMatrix, const Vec4& clipPlane, const Vec4& lightPlace) {
    GpuShadowView view{};
    writeColumnMajor(tileMatrix, view.matrix);
    view.clipPlane[0] = clipPlane.x;
    view.clipPlane[1] = clipPlane.y;
    view.clipPlane[2] = clipPlane.z;
    view.clipPlane[3] = clipPlane.w;
    view.lightPlace[0] = lightPlace.x;
    view.lightPlace[1] = lightPlace.y;
    view.lightPlace[2] = lightPlace.z;
    view.lightPlace[3] = lightPlace.w;
    return view;
}

GpuCaptureView packCaptureView(const Mat4& viewProjection, const Vec4& clipPlane, const Vec3& eye) {
    GpuCaptureView view{};
    writeColumnMajor(viewProjection, view.viewProjection);
    view.clipPlane[0] = clipPlane.x;
    view.clipPlane[1] = clipPlane.y;
    view.clipPlane[2] = clipPlane.z;
    view.clipPlane[3] = clipPlane.w;
    view.eye[0] = eye.x;
    view.eye[1] = eye.y;
    view.eye[2] = eye.z;
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
                           const ClusterGrid& clusters, bool clusteredShading, bool clusterHeatmap, const ShadowSettings& shadows) {
    GpuFrameData frame{};
    writeColumnMajor(relativeViewProjection, frame.relativeViewProjection);
    frame.cameraOrigin[0] = cameraOrigin.x;
    frame.cameraOrigin[1] = cameraOrigin.y;
    frame.cameraOrigin[2] = cameraOrigin.z;
    std::copy(lights.ambient, lights.ambient + 4, frame.ambient);
    std::copy(lights.counts, lights.counts + 4, frame.counts);
    const ShadowAtlasSize atlas = shadowAtlasSizeFor(shadows);
    frame.atlasTexel[0] = 1.0f / static_cast<float>(atlas.width);
    frame.atlasTexel[1] = 1.0f / static_cast<float>(atlas.height);
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
    frame.cascadeOptions[0] = shadows.cascadeBlendBand;
    frame.cascadeOptions[1] = shadows.cascadeTransition == CascadeTransition::Dither ? 1.0f : 0.0f;
    return frame;
}

/*
* GpuMaterial is all 4-byte fields with no padding (its size is checked), so its bytes can be hashed and compared as one
* block. Two materials pack the same exactly when they shade the same.
*/
std::uint32_t MaterialTable::add(const Material& material) {
    GpuMaterial packed = packMaterial(material);

    // A blended ground's layers are ordinary materials, added first (shared with any draw using the same), and named by
    // their places plus one.
    if (!material.groundLayers.empty()) {
        if (material.groundLayers.size() > 4 || !material.groundWeights.isValid()) {
            throw std::invalid_argument("A blended ground takes one to four layers and a weights texture");
        }

        if (material.groundPaint == GroundPaint::Strata && material.groundTopLayer >= static_cast<int>(material.groundLayers.size())) {
            throw std::invalid_argument("A blended ground's top layer must be one of its layers");
        }

        for (std::size_t layer = 0; layer < material.groundLayers.size(); ++layer) {
            if (!material.groundLayers[layer].groundLayers.empty()) {
                throw std::invalid_argument("A blended ground's layers may not be layered themselves");
            }

            packed.groundLayers[layer] = add(material.groundLayers[layer]) + 1;
        }
    }

    return addPacked(packed, materialHash(packed));
}

std::uint64_t materialHash(const GpuMaterial& packed) {
    Fnv1aHash hash;
    hash.addBytes(&packed, sizeof(packed));
    return hash.value;
}

std::uint32_t MaterialTable::addPacked(const GpuMaterial& packed, std::uint64_t hash) {
    for (std::size_t slot = 0; slot < recentFilled; ++slot) {
        if (recentHashes[slot] == hash && std::memcmp(&entries[recentIndices[slot]], &packed, sizeof(packed)) == 0) {
            return recentIndices[slot];
        }
    }

    std::uint32_t index = static_cast<std::uint32_t>(entries.size());
    bool found = false;
    const auto [first, last] = byHash.equal_range(hash);

    for (auto entry = first; entry != last && !found; ++entry) {
        if (std::memcmp(&entries[entry->second], &packed, sizeof(packed)) == 0) {
            index = entry->second;
            found = true;
        }
    }

    if (!found) {
        entries.push_back(packed);
        byHash.emplace(hash, index);
    }

    recentHashes[recentNext] = hash;
    recentIndices[recentNext] = index;
    recentNext = (recentNext + 1) % recentCount;
    recentFilled = std::min(recentFilled + 1, recentCount);
    return index;
}

std::vector<GpuInstance> packInstances(const std::vector<DrawItem>& draws, MaterialTable& materials, JobSystem& jobs) {
    constexpr std::size_t drawsPerPiece = 256;
    std::vector<GpuMaterial> packed(draws.size());
    std::vector<std::uint64_t> hashes(draws.size(), 0);
    jobs.parallelFor(draws.size(), drawsPerPiece, [&](std::size_t draw) {
        if (draws[draw].material.groundLayers.empty()) {
            packed[draw] = packMaterial(draws[draw].material);
            hashes[draw] = materialHash(packed[draw]);
        }
    });

    // In draw order, so the table's entries are what adding one by one would give; a blended ground adds its layers too.
    std::vector<std::uint32_t> indices(draws.size());

    for (std::size_t draw = 0; draw < draws.size(); ++draw) {
        indices[draw] = draws[draw].material.groundLayers.empty() ? materials.addPacked(packed[draw], hashes[draw])
                                                                   : materials.add(draws[draw].material);
    }

    std::vector<GpuInstance> instances(draws.size());
    jobs.parallelFor(draws.size(), drawsPerPiece, [&](std::size_t draw) {
        instances[draw] = packInstance(draws[draw], indices[draw]);
    });
    return instances;
}

const std::vector<GpuMaterial>& MaterialTable::getEntries() const {
    return entries;
}

/*
* A stable sort by style, then mesh, keeps each mesh's draws in the order they came, then each run of one mesh drawn one
* way becomes a batch. Cut-outs come last, so a pass switches pipeline at most once for them.
*/
/*
* Each chosen draw's mesh in this view is worked out once (a distance each), sorted with its draw, so the sort compares
* plain numbers.
*/
BatchedDraws batchByMesh(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen, std::uint32_t firstInstance,
                         const LevelView* levels) {
    const auto style = [&draws](std::size_t index) {
        const Material& material = draws[index].material;
        return (isCutOut(material) ? 2 : 0) + (material.doubleSided ? 1 : 0);
    };

    struct Keyed {
        int style = 0;
        std::uint32_t mesh = 0;
        std::size_t index = 0;
    };

    std::vector<Keyed> keyed(chosen.size());

    for (std::size_t position = 0; position < chosen.size(); ++position) {
        const std::size_t index = chosen[position];
        const MeshHandle mesh = levels != nullptr ? chosenMesh(draws[index], *levels) : draws[index].mesh;
        keyed[position] = Keyed{style(index), mesh.index, index};
    }

    std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
        return std::make_pair(a.style, a.mesh) < std::make_pair(b.style, b.mesh);
    });

    BatchedDraws batched;
    batched.order.reserve(keyed.size());

    for (std::size_t position = 0; position < keyed.size(); ++position) {
        const DrawItem& draw = draws[keyed[position].index];
        const MeshHandle mesh{keyed[position].mesh};
        const bool cutOut = isCutOut(draw.material);
        const bool doubleSided = draw.material.doubleSided;
        const DrawBatch* last = batched.batches.empty() ? nullptr : &batched.batches.back();
        batched.order.push_back(keyed[position].index);

        if (last == nullptr || last->mesh != mesh || last->cutOut != cutOut || last->doubleSided != doubleSided) {
            batched.batches.push_back(DrawBatch{mesh, firstInstance + static_cast<std::uint32_t>(position), 0, cutOut, doubleSided});
        }

        ++batched.batches.back().instanceCount;
    }

    return batched;
}
