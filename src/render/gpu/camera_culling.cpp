#include "render/gpu/camera_culling.h"
#include "math/angles.h"
#include "render/gpu/reflection_probe.h"
#include "render/gpu/reflection_probe_shared.h"
#include "scene/material.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>

namespace {
    // batchByMesh's ways of drawing: plain, double-sided, cut-out, double-sided cut-out.
    int drawStyle(const Material& material) {
        return (isCutOut(material) ? 2 : 0) + (material.doubleSided ? 1 : 0);
    }

    int styleOf(const CullGroupStyle& style) {
        return (style.cutOut ? 2 : 0) + (style.doubleSided ? 1 : 0);
    }

    // What decides a still draw's candidates and lists: its mesh, way of drawing, glass and flat faces.
    std::uint64_t candidateKey(const DrawItem& draw) {
        return (static_cast<std::uint64_t>(draw.mesh.index) << 8u) | (isTransparent(draw.material) ? 1u : 0u) | (draw.flatFaces.empty() ? 0u : 2u) |
               (static_cast<std::uint64_t>(drawStyle(draw.material)) << 2u);
    }

    bool sameMeshes(const GpuMeshLevels& a, const GpuMeshLevels& b) {
        return a.levels == b.levels && std::equal(std::begin(a.meshes), std::end(a.meshes), std::begin(b.meshes));
    }

    // The glass draws and the flat-faced draws not glass, of draws first to first + count.
    void listGlassAndFlat(std::span<const DrawItem> draws, std::size_t first, std::size_t count, std::vector<std::size_t>& glass,
                          std::vector<std::size_t>& flatFaced) {
        for (std::size_t draw = first; draw < first + count; ++draw) {
            if (isTransparent(draws[draw].material)) {
                glass.push_back(draw);
            } else if (!draws[draw].flatFaces.empty()) {
                flatFaced.push_back(draw);
            }
        }
    }

    // The levels of each mesh the draws name, into a table by mesh number grown to fit.
    void tableLevels(std::span<const DrawItem> draws, std::size_t first, std::size_t count, std::vector<GpuMeshLevels>& table) {
        for (std::size_t draw = first; draw < first + count; ++draw) {
            const DrawItem& item = draws[draw];

            if (!item.mesh.isValid()) {
                continue;
            }

            if (item.mesh.index >= table.size()) {
                table.resize(item.mesh.index + 1);
            }

            table[item.mesh.index] = packMeshLevels(item.mesh, item.levels);
        }
    }
}

/*
* Every candidate keyed by its group, then a stable sort by (style, mesh): the draws stay in their order within each,
* as batchByMesh's stable sort keeps them, and each run of one key becomes a group.
*/
CullGroups buildCullGroups(std::span<const DrawItem> draws, std::size_t first, std::size_t count,
                           const std::function<MeshPlacement(MeshHandle)>& placement) {
    struct Keyed {
        int style = 0;
        std::uint32_t mesh = 0;
        GpuCullCandidate candidate;
    };

    std::vector<Keyed> keyed;
    keyed.reserve(count);

    for (std::size_t draw = first; draw < first + count; ++draw) {
        const DrawItem& item = draws[draw];

        if (isTransparent(item.material) || !item.mesh.isValid()) {
            continue;
        }

        if (item.levels.size() >= meshLevelSlots) {
            throw std::invalid_argument("A mesh has at most three levels besides itself");
        }

        const int style = drawStyle(item.material);
        const std::uint32_t index = static_cast<std::uint32_t>(draw);
        keyed.push_back(Keyed{style, item.mesh.index, GpuCullCandidate{index, 0}});

        for (std::size_t level = 0; level < item.levels.size(); ++level) {
            if (item.levels[level].mesh.isValid()) {
                keyed.push_back(Keyed{style, item.levels[level].mesh.index, GpuCullCandidate{index, static_cast<std::uint32_t>(level + 1)}});
            }
        }
    }

    std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
        return std::make_pair(a.style, a.mesh) < std::make_pair(b.style, b.mesh);
    });

    CullGroups groups;
    groups.candidates.reserve(keyed.size());

    for (std::size_t position = 0; position < keyed.size(); ++position) {
        const Keyed& key = keyed[position];

        if (position == 0 || key.style != keyed[position - 1].style || key.mesh != keyed[position - 1].mesh) {
            const MeshHandle mesh{key.mesh};
            const MeshPlacement placed = placement(mesh);
            GpuCullGroup group;
            group.firstCandidate = static_cast<std::uint32_t>(position);
            group.indexCount = placed.indexCount;
            group.firstIndex = placed.firstIndex;
            group.vertexOffset = static_cast<std::int32_t>(placed.firstVertex);
            groups.groups.push_back(group);
            groups.styles.push_back(CullGroupStyle{mesh, (key.style & 2) != 0, (key.style & 1) != 0, placed.pooled});
        }

        ++groups.groups.back().candidates;
        groups.candidates.push_back(key.candidate);
    }

    return groups;
}

// A stable sort by (not pooled, style, mesh); each style's pooled groups then sit together.
CullDrawOrder cullDrawOrder(std::span<const CullGroupStyle> styles) {
    CullDrawOrder drawOrder;
    drawOrder.order.resize(styles.size());

    for (std::size_t group = 0; group < styles.size(); ++group) {
        drawOrder.order[group] = static_cast<std::uint32_t>(group);
    }

    const auto key = [&styles](std::uint32_t group) {
        return std::make_tuple(!styles[group].pooled, styleOf(styles[group]), styles[group].mesh.index);
    };
    std::stable_sort(drawOrder.order.begin(), drawOrder.order.end(), [&key](std::uint32_t a, std::uint32_t b) { return key(a) < key(b); });

    for (std::size_t position = 0; position < drawOrder.order.size(); ++position) {
        const CullGroupStyle& style = styles[drawOrder.order[position]];

        if (!style.pooled) {
            break;
        }

        const std::size_t run = static_cast<std::size_t>(styleOf(style));

        if (drawOrder.runCount[run] == 0) {
            drawOrder.runFirst[run] = static_cast<std::uint32_t>(position);
        }

        ++drawOrder.runCount[run];
        ++drawOrder.pooled;
    }

    return drawOrder;
}

GpuCameraCull packCameraCull(const Frustum& frustum, const LevelView& levels, bool motion, bool sway, float swayFadeEnd) {
    GpuCameraCull packed;
    packed.frustum = packFrustum(frustum);
    packed.levels = packLevelView(levels);
    packed.swayFadeEnd = swayFadeEnd;
    packed.flags = (motion ? CAMERA_CULL_MOTION : 0u) | (sway ? CAMERA_CULL_SWAY : 0u);
    return packed;
}

// The face's camera measured from the origin (as the capture draws it), moved back into the world for culling.
CaptureCullView probeFaceCullView(int face, const Vec3& origin, float farPlane, float faceSize, const LevelOfDetailSettings& settings) {
    const Mat4 worldView = cubeFaceViewProjection(face, static_cast<float>(PROBE_NEAR), farPlane) * Mat4::translation(-origin.x, -origin.y, -origin.z);
    return CaptureCullView{Frustum::fromClipMatrix(worldView, ClipDepth::OneToZero), levelView(origin, faceSize, 0.25f * fullTurnRadians, settings)};
}

// The mirror's levels are the camera's lens over the picture's height: the mirrored camera is as far from what it shows.
CaptureCullView mirrorCullView(const Mat4& mirrorViewProjection, const Vec3& cameraOrigin, const MirrorPlane& mirror, float pictureHeight,
                               float verticalFov, const LevelOfDetailSettings& settings) {
    const Mat4 worldView = mirrorViewProjection * Mat4::translation(-cameraOrigin.x, -cameraOrigin.y, -cameraOrigin.z);
    return CaptureCullView{planarFrustum(worldView, mirror), levelView(cameraOrigin, pictureHeight, verticalFov, settings)};
}

GpuCameraCull packCaptureCull(const CaptureCullView& capture) {
    return packCameraCull(capture.frustum, capture.levels, false, false, 0.0f);
}

// Every view's slices the same size, so a view's start is its number times one view's size.
void placeCullView(GpuCameraCull& view, std::uint32_t index, std::uint32_t candidates, const CullDrawOrder& order) {
    const CullViewSizes one = cullViewSizes(1, candidates, order);
    view.candidates = candidates;
    view.groups = static_cast<std::uint32_t>(order.order.size());
    view.blocks = (candidates + cameraCullGroupSize - 1) / cameraCullGroupSize;
    view.view = index;

    for (std::size_t run = 0; run < cullStyles; ++run) {
        view.runFirst[run] = order.runFirst[run];
        view.runCount[run] = order.runCount[run];
    }

    view.slices[CAMERA_CULL_SLICE_FLAGS] = static_cast<std::uint32_t>(index * one.flags);
    view.slices[CAMERA_CULL_SLICE_BLOCKS] = static_cast<std::uint32_t>(index * one.blocks);
    view.slices[CAMERA_CULL_SLICE_COMMANDS] = static_cast<std::uint32_t>(index * one.commands);
    view.slices[CAMERA_CULL_SLICE_PLACES] = static_cast<std::uint32_t>(index * one.places);
}

CullViewSizes cullViewSizes(std::size_t views, std::uint32_t candidates, const CullDrawOrder& order) {
    const std::size_t blocks = (static_cast<std::size_t>(candidates) + cameraCullGroupSize - 1) / cameraCullGroupSize;
    return CullViewSizes{views * candidates, views * (blocks + 1), views * order.order.size(), views * (static_cast<std::size_t>(order.pooled) + 1)};
}

bool swayReaches(const float bounds[4], const Vec3& eye, float fadeEnd) {
    const Vec3 offset{bounds[0] - eye.x, bounds[1] - eye.y, bounds[2] - eye.z};
    return offset.length() - std::max(bounds[3], 0.0f) < fadeEnd;
}

std::uint32_t cameraCandidateFlags(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuCameraCull& view, std::uint32_t slot) {
    if (!cameraTakes(record, view.frustum)) {
        return 0u;
    }

    const int chosen = chooseLevelSlot(record, levels, view.levels);

    if (chosen != static_cast<int>(slot)) {
        return 0u;
    }

    std::uint32_t flags = CAMERA_CULL_DRAWN;
    const Vec3 eye{view.levels.eye[0], view.levels.eye[1], view.levels.eye[2]};
    const bool sways = (view.flags & CAMERA_CULL_SWAY) != 0u && (record.flags & CULL_SWAYS) != 0u && swayReaches(record.bounds, eye, view.swayFadeEnd);

    if ((view.flags & CAMERA_CULL_MOTION) != 0u && ((record.flags & CULL_MOVED) != 0u || levels.changesShape != 0u || sways)) {
        flags |= CAMERA_CULL_MOVING;
    }

    if (slotThinned(record, levels, chosen)) {
        flags |= CAMERA_CULL_THINNED;
    }

    return flags;
}

/*
* As the three passes do it: each candidate's flags; their exclusive prefix sums (the GPU's are in blocks, which add
* up to the same numbers); each flagged candidate's entry at its sum; each group's command from the sums at its two ends.
*/
CameraCullResult cullCameraOnCpu(std::span<const GpuCullRecord> records, std::span<const GpuMeshLevels> levels, std::span<const GpuCullGroup> groups,
                                 std::span<const GpuCullCandidate> candidates, std::span<const std::uint32_t> drawOrder, const GpuCameraCull& view) {
    std::vector<std::uint32_t> flags(candidates.size());
    std::vector<std::uint32_t> mainSums(candidates.size() + 1, 0);
    std::vector<std::uint32_t> motionSums(candidates.size() + 1, 0);
    CameraCullResult result;

    for (std::size_t c = 0; c < candidates.size(); ++c) {
        const GpuCullRecord& record = records[candidates[c].draw];
        flags[c] = cameraCandidateFlags(record, levels[record.mesh], view, candidates[c].slot);
        mainSums[c + 1] = mainSums[c] + ((flags[c] & CAMERA_CULL_DRAWN) != 0u ? 1u : 0u);
        motionSums[c + 1] = motionSums[c] + ((flags[c] & CAMERA_CULL_MOVING) != 0u ? 1u : 0u);
        result.levels[candidates[c].slot] += (flags[c] & CAMERA_CULL_DRAWN) != 0u ? 1u : 0u;
    }

    result.mainEntries.resize(mainSums.back());
    result.motionEntries.resize(motionSums.back());

    for (std::size_t c = 0; c < candidates.size(); ++c) {
        const std::uint32_t entry = candidates[c].draw | ((flags[c] & CAMERA_CULL_THINNED) != 0u ? drawListThinnedBit : 0u);

        if ((flags[c] & CAMERA_CULL_DRAWN) != 0u) {
            result.mainEntries[mainSums[c]] = entry;
        }

        if ((flags[c] & CAMERA_CULL_MOVING) != 0u) {
            result.motionEntries[motionSums[c]] = entry;
        }
    }

    for (const GpuCullGroup& group : groups) {
        const std::size_t first = group.firstCandidate;
        const std::size_t end = first + group.candidates;
        result.mainCommands.push_back(GpuDrawCommand{group.indexCount, mainSums[end] - mainSums[first], group.firstIndex, group.vertexOffset,
                                                     view.mainBase + mainSums[first]});
        result.motionCommands.push_back(GpuDrawCommand{group.indexCount, motionSums[end] - motionSums[first], group.firstIndex, group.vertexOffset,
                                                       view.motionBase + motionSums[first]});
    }

    packCullCommands(result.mainCommands, drawOrder, view, result.packedMain, result.mainCounts);
    packCullCommands(result.motionCommands, drawOrder, view, result.packedMotion, result.motionCounts);
    return result;
}

// The packed list keeps the drawing order's length; places past a run's count are left empty.
void packCullCommands(std::span<const GpuDrawCommand> commands, std::span<const std::uint32_t> drawOrder, const GpuCameraCull& view,
                      std::vector<GpuDrawCommand>& packed, std::array<std::uint32_t, cullStyles>& counts) {
    packed.assign(drawOrder.size(), GpuDrawCommand{});
    counts.fill(0);

    for (std::size_t run = 0; run < cullStyles; ++run) {
        for (std::uint32_t position = view.runFirst[run]; position < view.runFirst[run] + view.runCount[run]; ++position) {
            const GpuDrawCommand& command = commands[drawOrder[position]];

            if (command.instanceCount > 0) {
                packed[view.runFirst[run] + counts[run]] = command;
                ++counts[run];
            }
        }
    }
}

bool nearACameraEdge(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuCameraCull& view) {
    if (nearAChoiceEdge(record, levels, view.levels, view.frustum)) {
        return true;
    }

    if ((view.flags & CAMERA_CULL_SWAY) == 0u || (record.flags & CULL_SWAYS) == 0u) {
        return false;
    }

    const Vec3 offset{record.bounds[0] - view.levels.eye[0], record.bounds[1] - view.levels.eye[1], record.bounds[2] - view.levels.eye[2]};
    return std::abs(offset.length() - std::max(record.bounds[3], 0.0f) - view.swayFadeEnd) < 1e-3f;
}

/*
* Both sides gathered by (mesh, style) and sorted, then walked together as multisets: what one has and the other not.
*/
CullListComparison compareCullList(std::span<const CullGroupStyle> styles, std::span<const GpuDrawCommand> commands,
                                   std::span<const std::uint32_t> entries, std::uint32_t base, const BatchedDraws& cpu,
                                   const std::function<bool(std::uint32_t)>& nearEdge) {
    using Key = std::tuple<std::uint32_t, int>;
    std::map<Key, std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>> byKey;

    for (std::size_t group = 0; group < commands.size() && group < styles.size(); ++group) {
        const GpuDrawCommand& command = commands[group];
        std::vector<std::uint32_t>& gpu = byKey[Key{styles[group].mesh.index, styleOf(styles[group])}].second;

        for (std::uint32_t i = 0; i < command.instanceCount; ++i) {
            const std::size_t at = static_cast<std::size_t>(command.firstInstance) + i;

            // An entry outside what was read back is a wrong command: counted as an entry no draw has.
            gpu.push_back(at >= base && at - base < entries.size() ? entries[at - base] : drawListIndexMask);
        }
    }

    const std::vector<std::uint32_t> cpuEntries = [&cpu] {
        std::vector<std::uint32_t> list;
        appendDrawList(list, cpu);
        return list;
    }();

    for (const DrawBatch& batch : cpu.batches) {
        std::vector<std::uint32_t>& list = byKey[Key{batch.mesh.index, (batch.cutOut ? 2 : 0) + (batch.doubleSided ? 1 : 0)}].first;
        list.insert(list.end(), cpuEntries.begin() + batch.firstInstance, cpuEntries.begin() + batch.firstInstance + batch.instanceCount);
    }

    CullListComparison comparison;
    comparison.entries = cpuEntries.size();

    for (auto& [key, lists] : byKey) {
        std::vector<std::uint32_t>& expected = lists.first;
        std::vector<std::uint32_t>& actual = lists.second;
        std::sort(expected.begin(), expected.end());
        std::sort(actual.begin(), actual.end());
        std::size_t e = 0;
        std::size_t a = 0;

        while (e < expected.size() || a < actual.size()) {
            if (a == actual.size() || (e < expected.size() && expected[e] < actual[a])) {
                ++comparison.missing;
                comparison.nearAnEdge += nearEdge(expected[e] & drawListIndexMask) ? 1 : 0;
                ++e;
            } else if (e == expected.size() || actual[a] < expected[e]) {
                ++comparison.extra;
                comparison.nearAnEdge += nearEdge(actual[a] & drawListIndexMask) ? 1 : 0;
                ++a;
            } else {
                ++comparison.matching;
                ++e;
                ++a;
            }
        }
    }

    return comparison;
}

namespace {
    std::string describeList(const char* name, const CullListComparison& list) {
        return std::string{name} + ": " + std::to_string(list.entries) + " entries, " + std::to_string(list.matching) + " matching, " +
               std::to_string(list.missing) + " missing, " + std::to_string(list.extra) + " extra (" + std::to_string(list.nearAnEdge) +
               " near an edge)";
    }

    bool agrees(const CullListComparison& list) {
        return list.missing + list.extra == list.nearAnEdge;
    }
}

std::string describeCullComparison(std::span<const NamedCullComparison> lists) {
    std::string reply;
    bool allAgree = true;

    for (const NamedCullComparison& named : lists) {
        reply += describeList(named.name.c_str(), named.list) + ". ";
        allAgree = allAgree && agrees(named.list);
    }

    return reply + (allAgree ? "The GPU's lists agree with the CPU's." : "The GPU's lists DIFFER from the CPU's.");
}

/*
* Kept when the frame is the one packed; built ranges repacked when it follows it; otherwise built afresh. The moving
* draws' part is made every frame.
*/
CameraCullFrame CameraCullTable::update(const FrameDescription& frame, const std::function<MeshPlacement(MeshHandle)>& placement,
                                        const std::function<bool(MeshHandle)>& changesShape) {
    const std::size_t stillCount = std::min(frame.stillDraws, frame.draws.size());

    if (packedVersion != frame.stillVersion || records.size() != stillCount || (stillCount == 0 && generation == 0)) {
        const bool follows = packedVersion != 0 && packedVersion == frame.stillPreviousVersion && records.size() == stillCount && stillCount > 0;

        if (!follows || !repackRanges(frame)) {
            buildStill(frame, placement);
        }

        packedVersion = frame.stillVersion;
        ++generation;
    }

    CameraCullFrame result;
    const std::size_t movingCount = frame.draws.size() - stillCount;
    result.stillRecords = records;
    result.stillCandidates = still.candidates;
    result.stillGeneration = generation;
    result.movingRecords.reserve(movingCount);

    for (std::size_t draw = stillCount; draw < frame.draws.size(); ++draw) {
        result.movingRecords.push_back(packCullRecord(frame.draws[draw]));
    }

    CullGroups moving = buildCullGroups(frame.draws, stillCount, movingCount, placement);
    const std::uint32_t stillCandidates = static_cast<std::uint32_t>(still.candidates.size());

    for (GpuCullGroup& group : moving.groups) {
        group.firstCandidate += stillCandidates;
    }

    result.movingCandidates = std::move(moving.candidates);
    result.groups = still.groups;
    result.groups.insert(result.groups.end(), moving.groups.begin(), moving.groups.end());
    result.styles = still.styles;
    result.styles.insert(result.styles.end(), moving.styles.begin(), moving.styles.end());

    result.drawOrder = cullDrawOrder(result.styles);

    result.meshLevels = stillLevels;
    tableLevels(frame.draws, stillCount, movingCount, result.meshLevels);

    for (std::size_t mesh = 0; mesh < result.meshLevels.size(); ++mesh) {
        result.meshLevels[mesh].changesShape = changesShape(MeshHandle{static_cast<std::uint32_t>(mesh)}) ? 1u : 0u;
    }

    result.glass = stillGlass;
    result.flatFaced = stillFlatFaced;
    listGlassAndFlat(frame.draws, stillCount, movingCount, result.glass, result.flatFaced);
    return result;
}

std::size_t CameraCullTable::getStillBuilds() const {
    return stillBuilds;
}

void CameraCullTable::buildStill(const FrameDescription& frame, const std::function<MeshPlacement(MeshHandle)>& placement) {
    const std::size_t stillCount = std::min(frame.stillDraws, frame.draws.size());
    records.clear();
    keys.clear();
    records.reserve(stillCount);
    keys.reserve(stillCount);

    for (std::size_t draw = 0; draw < stillCount; ++draw) {
        records.push_back(packCullRecord(frame.draws[draw]));
        keys.push_back(candidateKey(frame.draws[draw]));
    }

    still = buildCullGroups(frame.draws, 0, stillCount, placement);
    stillLevels.clear();
    tableLevels(frame.draws, 0, stillCount, stillLevels);
    stillGlass.clear();
    stillFlatFaced.clear();
    listGlassAndFlat(frame.draws, 0, stillCount, stillGlass, stillFlatFaced);
    ++stillBuilds;
}

/*
* Each built draw's record packed again; false (build everything) where its candidates or lists would change: its key,
* or its mesh's levels' meshes.
*/
bool CameraCullTable::repackRanges(const FrameDescription& frame) {
    for (const DrawRange& range : frame.builtStillDraws) {
        if (range.first + range.count > records.size()) {
            return false;
        }

        for (std::size_t draw = range.first; draw < range.first + range.count; ++draw) {
            const DrawItem& item = frame.draws[draw];

            if (candidateKey(item) != keys[draw]) {
                return false;
            }

            if (item.mesh.isValid()) {
                if (item.mesh.index >= stillLevels.size()) {
                    return false;
                }

                const GpuMeshLevels packed = packMeshLevels(item.mesh, item.levels);

                if (!sameMeshes(packed, stillLevels[item.mesh.index])) {
                    return false;
                }

                stillLevels[item.mesh.index] = packed;
            }

            records[draw] = packCullRecord(item);
        }
    }

    return true;
}
