#include "render/gpu/gpu_scene.h"
#include "core/hash.h"
#include "render/gpu/column_major.h"
#include "render/gpu/shadow_map.h"
#include "render/gpu/wind.h"

#include <algorithm>
#include <bit>
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
    instance.material[3] = draw.material.impostorFrames > 0 ? instanceKindImpostor : draw.followsGround ? instanceKindFollowsGround : instanceKindPlain;
    const Vec4 sway = packSway(draw.sway);
    instance.sway[0] = sway.x;
    instance.sway[1] = sway.y;
    instance.sway[2] = sway.z;
    instance.sway[3] = sway.w;
    instance.hanging[0] = draw.hangingEntry;
    instance.hanging[1] = std::bit_cast<std::uint32_t>(draw.sway.swing);
    instance.hanging[2] = draw.thinningEntry;
    const BoundingSphere& choosing = draw.levelBounds.radius >= 0.0f ? draw.levelBounds : draw.bounds;
    instance.levelSphere[0] = choosing.centre.x;
    instance.levelSphere[1] = choosing.centre.y;
    instance.levelSphere[2] = choosing.centre.z;
    instance.levelSphere[3] = choosing.radius;
    return instance;
}

// Column-major like every matrix the shaders read, for vkCmdPushConstants.
GpuShadowView packShadowView(const Mat4& tileMatrix, const Vec4& clipPlane, const Vec4& lightPlace, bool sways) {
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
    view.options[0] = sways ? 1.0f : 0.0f;
    return view;
}

GpuCaptureView packCaptureView(const Mat4& viewProjection, const Vec4& clipPlane, const Vec3& eye, float thinningPerMetre) {
    GpuCaptureView view{};
    writeColumnMajor(viewProjection, view.viewProjection);
    view.clipPlane[0] = clipPlane.x;
    view.clipPlane[1] = clipPlane.y;
    view.clipPlane[2] = clipPlane.z;
    view.clipPlane[3] = clipPlane.w;
    view.eye[0] = eye.x;
    view.eye[1] = eye.y;
    view.eye[2] = eye.z;
    view.eye[3] = thinningPerMetre;
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
namespace {
    // A blended ground takes one to four plain layers and a weights texture, and a strata ground's top layer is one of them.
    void checkBlendedGround(const Material& material) {
        if (material.groundLayers.size() > 4 || !material.groundWeights.isValid()) {
            throw std::invalid_argument("A blended ground takes one to four layers and a weights texture");
        }

        if (material.groundPaint == GroundPaint::Strata && material.groundTopLayer >= static_cast<int>(material.groundLayers.size())) {
            throw std::invalid_argument("A blended ground's top layer must be one of its layers");
        }

        for (const Material& layer : material.groundLayers) {
            if (!layer.groundLayers.empty()) {
                throw std::invalid_argument("A blended ground's layers may not be layered themselves");
            }
        }
    }
}

std::uint32_t MaterialTable::add(const Material& material) {
    GpuMaterial packed = packMaterial(material);

    // A blended ground's layers are ordinary materials, added first (shared with any draw using the same), and named by
    // their places plus one.
    if (!material.groundLayers.empty()) {
        checkBlendedGround(material);

        for (std::size_t layer = 0; layer < material.groundLayers.size(); ++layer) {
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

MaterialTable MaterialTable::over(const MaterialTable& base) {
    MaterialTable table;
    table.base = &base;
    table.firstIndex = base.firstIndex + static_cast<std::uint32_t>(base.entries.size());
    return table;
}

std::optional<std::uint32_t> MaterialTable::findOwn(const GpuMaterial& packed, std::uint64_t hash) const {
    const auto [first, last] = byHash.equal_range(hash);

    for (auto entry = first; entry != last; ++entry) {
        if (std::memcmp(&entries[entry->second - firstIndex], &packed, sizeof(packed)) == 0) {
            return entry->second;
        }
    }

    return std::nullopt;
}

// The recent entries first, then the base's (and its bases'), then this table's own; numbers are the GPU's (base first).
std::uint32_t MaterialTable::addPacked(const GpuMaterial& packed, std::uint64_t hash) {
    for (std::size_t slot = 0; slot < recentFilled; ++slot) {
        if (recentHashes[slot] == hash && std::memcmp(&at(recentIndices[slot]), &packed, sizeof(packed)) == 0) {
            return recentIndices[slot];
        }
    }

    std::optional<std::uint32_t> found;

    for (const MaterialTable* below = base; below != nullptr && !found; below = below->base) {
        found = below->findOwn(packed, hash);
    }

    if (!found) {
        found = findOwn(packed, hash);
    }

    const std::uint32_t index = found.value_or(firstIndex + static_cast<std::uint32_t>(entries.size()));

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

/*
* Three passes. On every core: each material packed (a blended ground's own bytes with its layers' places empty), plain
* ones hashed, a ground's layers packed and hashed into their own list. On every core: whether each draw's packed bytes
* (and a ground's layers') equal the draw before's, as the terrain's patches of one region do in runs. On one thread, in
* draw order: a draw like the one before takes its number; any other is added, a ground's layers first (checked as
* MaterialTable::add checks them), then the ground with their places, so the table is what adding one by one gives.
* The one thread's work follows the runs, not the draws: 699 terrain patches packed one by one cost 1.55 ms.
*/
std::vector<GpuInstance> packInstances(std::span<const DrawItem> draws, MaterialTable& materials, JobSystem& jobs) {
    constexpr std::size_t drawsPerPiece = 256;

    // A ground with four layers packs five materials (about 2 us): smaller pieces, so the terrain's few hundred patches
    // spread over every core rather than three.
    constexpr std::size_t packedPerPiece = 32;
    const auto isGround = [&](std::size_t draw) { return !draws[draw].material.groundLayers.empty(); };

    // Each ground's first layer in the layer lists.
    std::vector<std::size_t> firstLayer(draws.size() + 1, 0);

    for (std::size_t draw = 0; draw < draws.size(); ++draw) {
        firstLayer[draw + 1] = firstLayer[draw] + draws[draw].material.groundLayers.size();
    }

    std::vector<GpuMaterial> packed(draws.size());
    std::vector<std::uint64_t> hashes(draws.size(), 0);
    std::vector<GpuMaterial> layers(firstLayer.back());
    std::vector<std::uint64_t> layerHashes(firstLayer.back(), 0);
    jobs.parallelFor(draws.size(), packedPerPiece, [&](std::size_t draw) {
        const Material& material = draws[draw].material;
        packed[draw] = packMaterial(material);

        if (!isGround(draw)) {
            hashes[draw] = materialHash(packed[draw]);
            return;
        }

        for (std::size_t layer = 0; layer < material.groundLayers.size(); ++layer) {
            layers[firstLayer[draw] + layer] = packMaterial(material.groundLayers[layer]);
            layerHashes[firstLayer[draw] + layer] = materialHash(layers[firstLayer[draw] + layer]);
        }
    });

    std::vector<std::uint8_t> likeBefore(draws.size(), 0);
    jobs.parallelFor(draws.size(), drawsPerPiece, [&](std::size_t draw) {
        if (draw == 0 || isGround(draw) != isGround(draw - 1) || std::memcmp(&packed[draw], &packed[draw - 1], sizeof(GpuMaterial)) != 0) {
            return;
        }

        const std::size_t layerCount = firstLayer[draw + 1] - firstLayer[draw];
        const bool sameLayers = layerCount == firstLayer[draw] - firstLayer[draw - 1] &&
                                (layerCount == 0 || std::memcmp(&layers[firstLayer[draw]], &layers[firstLayer[draw - 1]], layerCount * sizeof(GpuMaterial)) == 0);
        likeBefore[draw] = sameLayers ? 1 : 0;
    });

    std::vector<std::uint32_t> indices(draws.size());

    for (std::size_t draw = 0; draw < draws.size(); ++draw) {
        if (likeBefore[draw] != 0) {
            indices[draw] = indices[draw - 1];
        } else if (!isGround(draw)) {
            indices[draw] = materials.addPacked(packed[draw], hashes[draw]);
        } else {
            checkBlendedGround(draws[draw].material);
            GpuMaterial ground = packed[draw];

            for (std::size_t layer = 0; layer < firstLayer[draw + 1] - firstLayer[draw]; ++layer) {
                ground.groundLayers[layer] = materials.addPacked(layers[firstLayer[draw] + layer], layerHashes[firstLayer[draw] + layer]) + 1;
            }

            indices[draw] = materials.addPacked(ground, materialHash(ground));
        }
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

const GpuMaterial& MaterialTable::at(std::uint32_t index) const {
    if (index < firstIndex) {
        return base->at(index);
    }

    return entries.at(index - firstIndex);
}

std::uint32_t MaterialTable::getFirstIndex() const {
    return firstIndex;
}

/*
* A stable sort by style, then mesh, keeps each mesh's draws in the order they came, then each run of one mesh drawn one
* way becomes a batch. Cut-outs come last, so a pass switches pipeline at most once for them.
*/
/*
* Each chosen draw's mesh in this view is worked out once (a distance each), sorted with its draw, so the sort compares
* plain numbers.
*/
namespace {
    // Drawn with its thinned mesh: the mesh itself, or a level sharing its first vertices, of a draw with thinning records.
    bool drawnThinned(const DrawItem& draw, MeshHandle mesh) {
        return draw.thinningEntry != 0 &&
               (mesh == draw.mesh || std::any_of(draw.levels.begin(), draw.levels.end(), [mesh](const MeshLevel& level) { return level.mesh == mesh && level.sharesVertices; }));
    }
}

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
        bool thinned = false;
    };

    std::vector<Keyed> keyed;
    keyed.reserve(chosen.size());

    for (std::size_t position = 0; position < chosen.size(); ++position) {
        const std::size_t index = chosen[position];
        const MeshHandle mesh = levels != nullptr ? chosenMesh(draws[index], *levels) : draws[index].mesh;

        if (!mesh.isValid()) {
            continue;
        }

        keyed.push_back(Keyed{style(index), mesh.index, index, drawnThinned(draws[index], mesh)});
    }

    std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
        return std::make_pair(a.style, a.mesh) < std::make_pair(b.style, b.mesh);
    });

    BatchedDraws batched;
    batched.order.reserve(keyed.size());
    batched.thinned.reserve(keyed.size());

    for (std::size_t position = 0; position < keyed.size(); ++position) {
        const DrawItem& draw = draws[keyed[position].index];
        const MeshHandle mesh{keyed[position].mesh};
        const bool cutOut = isCutOut(draw.material);
        const bool doubleSided = draw.material.doubleSided;
        const DrawBatch* last = batched.batches.empty() ? nullptr : &batched.batches.back();
        batched.order.push_back(keyed[position].index);
        batched.thinned.push_back(keyed[position].thinned);

        if (last == nullptr || last->mesh != mesh || last->cutOut != cutOut || last->doubleSided != doubleSided) {
            batched.batches.push_back(DrawBatch{mesh, firstInstance + static_cast<std::uint32_t>(position), 0, cutOut, doubleSided});
        }

        ++batched.batches.back().instanceCount;
    }

    return batched;
}

void appendDrawList(std::vector<std::uint32_t>& list, const BatchedDraws& batched) {
    for (std::size_t position = 0; position < batched.order.size(); ++position) {
        const std::uint32_t instance = static_cast<std::uint32_t>(batched.order[position]);
        list.push_back(position < batched.thinned.size() && batched.thinned[position] ? instance | drawListThinnedBit : instance);
    }
}

std::uint32_t DrawListLayout::reserve(std::size_t count) {
    const std::uint32_t first = entries;
    entries += static_cast<std::uint32_t>(count);
    return first;
}

// Entries straight after the last run continue it, so each write is one copy.
std::uint32_t DrawListLayout::append(std::span<const std::uint32_t> list) {
    const std::uint32_t first = entries;

    if (list.empty()) {
        return first;
    }

    if (runs.empty() || runs.back().first + runs.back().entries.size() != first) {
        runs.push_back(Run{first, {}});
    }

    runs.back().entries.insert(runs.back().entries.end(), list.begin(), list.end());
    entries += static_cast<std::uint32_t>(list.size());
    return first;
}

std::uint32_t DrawListLayout::size() const {
    return entries;
}

const std::vector<DrawListLayout::Run>& DrawListLayout::getRuns() const {
    return runs;
}

std::vector<GpuThinningRecord> packCardThinning(const IndexedMesh& mesh) {
    if (mesh.thinningCards.empty()) {
        return {};
    }

    if (mesh.thinningCards.size() != mesh.vertices.size()) {
        throw std::invalid_argument("A thinned mesh needs one card record a vertex");
    }

    std::vector<GpuThinningRecord> records;
    records.reserve(mesh.thinningCards.size() + 1);
    records.push_back(GpuThinningRecord{{mesh.thinning.cardSize, mesh.thinning.growth, mesh.thinning.leastShare, 0.0f}});

    for (const ThinningCard& card : mesh.thinningCards) {
        records.push_back(GpuThinningRecord{{card.anchor.x, card.anchor.y, card.anchor.z, card.rank}});
    }

    return records;
}
