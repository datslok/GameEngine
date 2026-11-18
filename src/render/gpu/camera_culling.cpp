#include "render/gpu/camera_culling.h"
#include "math/angles.h"
#include "render/gpu/level_fade.h"
#include "render/gpu/reflection_probe.h"
#include "render/gpu/reflection_probe_shared.h"
#include "scene/material.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
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
            group.firstSegment = static_cast<std::uint32_t>(groups.segments.size());
            group.segments = 1;
            group.indexCount = placed.indexCount;
            group.firstIndex = placed.firstIndex;
            group.vertexOffset = static_cast<std::int32_t>(placed.firstVertex);
            groups.groups.push_back(group);
            groups.styles.push_back(CullGroupStyle{mesh, (key.style & 2) != 0, (key.style & 1) != 0, placed.pooled});
            groups.segments.push_back(GpuCullSegment{static_cast<std::uint32_t>(position), 0, cullNoChunk, 0});
        }

        ++groups.segments.back().candidates;
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
void placeCullView(GpuCameraCull& view, std::uint32_t index, std::uint32_t candidates, std::uint32_t segments, const CullDrawOrder& order) {
    const CullViewSizes one = cullViewSizes(1, candidates, segments, order);
    view.candidates = candidates;
    view.groups = static_cast<std::uint32_t>(order.order.size());
    view.blocks = (candidates + cameraCullGroupSize - 1) / cameraCullGroupSize;
    view.segments = segments;
    view.segmentBlocks = (segments + cameraCullGroupSize - 1) / cameraCullGroupSize;
    view.view = index;

    for (std::size_t run = 0; run < cullStyles; ++run) {
        view.runFirst[run] = order.runFirst[run];
        view.runCount[run] = order.runCount[run];
    }

    const std::uint32_t inBatch = index % static_cast<std::uint32_t>(cullScratchViews);
    view.slices[CAMERA_CULL_SLICE_FLAGS] = static_cast<std::uint32_t>(inBatch * one.flags);
    view.slices[CAMERA_CULL_SLICE_BLOCKS] = static_cast<std::uint32_t>(inBatch * one.blocks);
    view.slices[CAMERA_CULL_SLICE_COMMANDS] = static_cast<std::uint32_t>(index * one.commands);
    view.slices[CAMERA_CULL_SLICE_PLACES] = static_cast<std::uint32_t>(index * one.places);
    view.segmentSlices[CAMERA_CULL_SEGMENT_SLICE_OFFSETS] = static_cast<std::uint32_t>(inBatch * one.segments);
    view.segmentSlices[CAMERA_CULL_SEGMENT_SLICE_BLOCKS] = static_cast<std::uint32_t>(inBatch * one.segmentBlocks);
}

CullViewSizes cullViewSizes(std::size_t views, std::uint32_t candidates, std::uint32_t segments, const CullDrawOrder& order) {
    const std::size_t blocks = (static_cast<std::size_t>(candidates) + cameraCullGroupSize - 1) / cameraCullGroupSize;
    const std::size_t segmentBlocks = (static_cast<std::size_t>(segments) + cameraCullGroupSize - 1) / cameraCullGroupSize;
    const std::size_t scratch = std::min(views, cullScratchViews);
    return CullViewSizes{scratch * candidates,
                         scratch * (blocks + 1),
                         views * order.order.size(),
                         views * (static_cast<std::size_t>(order.pooled) + 1),
                         scratch * segments,
                         scratch * (segmentBlocks + 1)};
}

bool boxInFrustum(const GpuFrustum& frustum, const float minimum[3], const float maximum[3]) {
    for (const auto& plane : frustum.planes) {
        const float x = plane[0] >= 0.0f ? maximum[0] : minimum[0];
        const float y = plane[1] >= 0.0f ? maximum[1] : minimum[1];
        const float z = plane[2] >= 0.0f ? maximum[2] : minimum[2];

        if (plane[0] * x + plane[1] * y + plane[2] * z + plane[3] < 0.0f) {
            return false;
        }
    }

    return true;
}

// The reach: the box's nearest point to the light's centre, within its radius (squared, as shadowTakes's spheres).
bool chunkInView(const GpuCullChunk& chunk, const GpuCameraCull& view) {
    const bool shadow = (view.flags & CAMERA_CULL_SHADOW) != 0u;

    if ((chunk.flags & (shadow ? CULL_CHUNK_CASTS : CULL_CHUNK_SEEN)) == 0u) {
        return false;
    }

    if ((chunk.flags & CULL_CHUNK_ALWAYS) != 0u) {
        return true;
    }

    if (shadow && view.reach[3] >= 0.0f) {
        float distanceSquared = 0.0f;

        for (int axis = 0; axis < 3; ++axis) {
            const float nearest = std::clamp(view.reach[axis], chunk.minimum[axis], chunk.maximum[axis]);
            const float gap = view.reach[axis] - nearest;
            distanceSquared += gap * gap;
        }

        if (distanceSquared > view.reach[3] * view.reach[3]) {
            return false;
        }
    }

    return boxInFrustum(view.frustum, chunk.minimum, chunk.maximum);
}

bool swayReaches(const float bounds[4], const Vec3& eye, float fadeEnd) {
    const Vec3 offset{bounds[0] - eye.x, bounds[1] - eye.y, bounds[2] - eye.z};
    return offset.length() - std::max(bounds[3], 0.0f) < fadeEnd;
}

// The CPU's steps in its order: the kind of draw, the reach (squared distances, as castersInReach), the view, the size,
// the cascade.
bool shadowTakes(const GpuCullRecord& record, const GpuCameraCull& view) {
    if ((record.flags & (CULL_TRANSPARENT | CULL_HIDDEN)) != 0u || (record.flags & CULL_CASTS_SHADOWS) == 0u) {
        return false;
    }

    if (view.reach[3] >= 0.0f && record.bounds[3] >= 0.0f) {
        const float touching = view.reach[3] + record.bounds[3];
        const Vec3 offset{record.bounds[0] - view.reach[0], record.bounds[1] - view.reach[1], record.bounds[2] - view.reach[2]};

        if (offset.lengthSquared() >= touching * touching) {
            return false;
        }
    }

    if (!sphereInFrustum(view.frustum, record.bounds)) {
        return false;
    }

    if (record.bounds[3] >= 0.0f && record.bounds[3] < view.smallestCaster) {
        return false;
    }

    return view.cascade < 0 || record.shadowCascades <= 0 || view.cascade < record.shadowCascades;
}

std::uint32_t cameraCandidateFlags(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuCameraCull& view, std::uint32_t slot) {
    const bool shadow = (view.flags & CAMERA_CULL_SHADOW) != 0u;

    if (shadow ? !shadowTakes(record, view) : !cameraTakes(record, view.frustum)) {
        return 0u;
    }

    // A shadow tile keeping its movers apart lists those that moved second; its shaders thin nothing, and nothing fades.
    if (shadow) {
        if (chooseLevelSlot(record, levels, view.levels) != static_cast<int>(slot)) {
            return 0u;
        }

        const bool apart = (view.flags & CAMERA_CULL_MOTION) != 0u && (record.flags & CULL_MOVED) != 0u;
        return apart ? CAMERA_CULL_MOVING : CAMERA_CULL_DRAWN;
    }

    // Fading, the coarser slot is drawn too (the finer is the one chosen).
    const GpuLevelFade fade = levelFadeSlots(record, levels, view.levels);
    const bool chosen = fade.fading ? fade.finer == static_cast<int>(slot) || fade.coarser == static_cast<int>(slot)
                                    : chooseLevelSlot(record, levels, view.levels) == static_cast<int>(slot);

    if (!chosen) {
        return 0u;
    }

    std::uint32_t flags = CAMERA_CULL_DRAWN | (fade.fading ? CAMERA_CULL_FADES : 0u);
    const Vec3 eye{view.levels.eye[0], view.levels.eye[1], view.levels.eye[2]};
    const bool sways = (view.flags & CAMERA_CULL_SWAY) != 0u && (record.flags & CULL_SWAYS) != 0u && swayReaches(record.bounds, eye, view.swayFadeEnd);

    if ((view.flags & CAMERA_CULL_MOTION) != 0u && ((record.flags & CULL_MOVED) != 0u || levels.changesShape != 0u || sways)) {
        flags |= CAMERA_CULL_MOVING;
    }

    if (slotThinned(record, levels, static_cast<int>(slot))) {
        flags |= CAMERA_CULL_THINNED;
    }

    return flags;
}

std::uint32_t cameraCandidateFade(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuCameraCull& view, std::uint32_t slot) {
    const GpuLevelFade fade = (view.flags & CAMERA_CULL_SHADOW) != 0u ? GpuLevelFade{} : levelFadeSlots(record, levels, view.levels);
    return fade.fading ? levelFadeField(fade.share, fade.coarser == static_cast<int>(slot)) : 0u;
}

std::uint32_t cameraCandidateEntry(std::uint32_t draw, std::uint32_t flags, std::uint32_t fade) {
    return draw | ((flags & CAMERA_CULL_THINNED) != 0u ? drawListThinnedBit : 0u) | (fade << drawListFadeShift);
}

/*
* As the three passes do it: each candidate's flags; their exclusive prefix sums (the GPU's are in blocks, which add
* up to the same numbers); each flagged candidate's entry at its sum; each group's command from the sums at its two ends.
*/
CameraCullResult cullCameraOnCpu(const CullTables& tables, std::span<const std::uint32_t> drawOrder, const GpuCameraCull& view) {
    // Each segment's first place among the visible candidates (the total after the last), and the visible candidates.
    std::vector<std::uint32_t> segmentStarts(tables.segments.size() + 1, 0);
    std::vector<std::uint32_t> visible;

    for (std::size_t segment = 0; segment < tables.segments.size(); ++segment) {
        const GpuCullSegment& run = tables.segments[segment];
        const bool seen = run.chunk == cullNoChunk || chunkInView(tables.chunks[run.chunk], view);
        segmentStarts[segment + 1] = segmentStarts[segment] + (seen ? run.candidates : 0u);

        for (std::uint32_t candidate = 0; seen && candidate < run.candidates; ++candidate) {
            visible.push_back(run.firstCandidate + candidate);
        }
    }

    std::vector<std::uint32_t> flags(visible.size());
    std::vector<std::uint32_t> mainSums(visible.size() + 1, 0);
    std::vector<std::uint32_t> motionSums(visible.size() + 1, 0);
    CameraCullResult result;

    for (std::size_t c = 0; c < visible.size(); ++c) {
        const GpuCullCandidate& candidate = tables.candidates[visible[c]];
        const GpuCullRecord& record = tables.records[candidate.draw];
        flags[c] = cameraCandidateFlags(record, tables.levels[record.mesh], view, candidate.slot);
        mainSums[c + 1] = mainSums[c] + ((flags[c] & CAMERA_CULL_DRAWN) != 0u ? 1u : 0u);
        motionSums[c + 1] = motionSums[c] + ((flags[c] & CAMERA_CULL_MOVING) != 0u ? 1u : 0u);
        result.levels[candidate.slot] += (flags[c] & CAMERA_CULL_DRAWN) != 0u ? 1u : 0u;
    }

    result.mainEntries.resize(mainSums.back());
    result.motionEntries.resize(motionSums.back());

    for (std::size_t c = 0; c < visible.size(); ++c) {
        const GpuCullCandidate& candidate = tables.candidates[visible[c]];
        const GpuCullRecord& record = tables.records[candidate.draw];
        const std::uint32_t fade = (flags[c] & CAMERA_CULL_FADES) != 0u ? cameraCandidateFade(record, tables.levels[record.mesh], view, candidate.slot) : 0u;
        const std::uint32_t entry = cameraCandidateEntry(candidate.draw, flags[c], fade);

        if ((flags[c] & CAMERA_CULL_DRAWN) != 0u) {
            result.mainEntries[mainSums[c]] = entry;
        }

        if ((flags[c] & CAMERA_CULL_MOVING) != 0u) {
            result.motionEntries[motionSums[c]] = entry;
        }
    }

    for (const GpuCullGroup& group : tables.groups) {
        const std::size_t first = segmentStarts[group.firstSegment];
        const std::size_t end = segmentStarts[group.firstSegment + group.segments];
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

    // A shadow tile's own edges: the sphere touching the reach within a millimetre, or the smallest caster's size.
    if ((view.flags & CAMERA_CULL_SHADOW) != 0u) {
        const Vec3 offset{record.bounds[0] - view.reach[0], record.bounds[1] - view.reach[1], record.bounds[2] - view.reach[2]};
        const bool atReach = view.reach[3] >= 0.0f && record.bounds[3] >= 0.0f && std::abs(offset.length() - view.reach[3] - record.bounds[3]) < 1e-3f;
        return atReach || std::abs(record.bounds[3] - view.smallestCaster) < 1e-4f;
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

CameraCullFrame::Joined CameraCullFrame::join() const {
    Joined joined;
    joined.records.assign(stillRecords.begin(), stillRecords.end());
    joined.records.insert(joined.records.end(), movingRecords.begin(), movingRecords.end());
    joined.candidates.assign(stillCandidates.begin(), stillCandidates.end());
    joined.candidates.insert(joined.candidates.end(), movingCandidates.begin(), movingCandidates.end());
    joined.segments.assign(stillSegments.begin(), stillSegments.end());
    joined.segments.insert(joined.segments.end(), movingSegments.begin(), movingSegments.end());
    return joined;
}

CullTables CameraCullFrame::tables(const Joined& joined) const {
    return CullTables{joined.records, meshLevels, stillChunks, groups, joined.segments, joined.candidates};
}

namespace {
    // A group's key: its style above its mesh, so keys sort as batchByMesh's groups do.
    std::uint64_t groupKey(int style, std::uint32_t mesh) {
        return (static_cast<std::uint64_t>(style) << 32u) | mesh;
    }

    // Room a chunk's candidates take: a quarter spare, so a change rarely moves the run.
    std::size_t candidateRoom(std::size_t count) {
        constexpr std::size_t fewest = 8;
        return std::max(count + count / 4, fewest);
    }

    // Sorted indices joined into ranges of consecutive ones.
    std::vector<DrawRange> rangesOf(const std::set<std::size_t>& indices) {
        std::vector<DrawRange> ranges;

        for (const std::size_t index : indices) {
            if (!ranges.empty() && ranges.back().first + ranges.back().count == index) {
                ++ranges.back().count;
            } else {
                ranges.push_back(DrawRange{index, 1});
            }
        }

        return ranges;
    }
}

/*
* Kept when the frame is the one packed; built ranges repacked when it follows it (with the same chunk size); otherwise
* built afresh. The moving draws' part is made every frame, after the still part.
*/
CameraCullFrame CameraCullTable::update(const FrameDescription& frame, const std::function<MeshPlacement(MeshHandle)>& placement,
                                        const std::function<bool(MeshHandle)>& changesShape) {
    // A draw list entry names its instance in its low bits, the rest its marks (instance_shared.h).
    if (frame.draws.size() > drawListInstances) {
        throw std::length_error("A frame has more draws than a draw list entry can name");
    }

    const std::size_t stillCount = std::min(frame.stillDraws, frame.draws.size());
    const std::size_t frameChunkSlots = std::max<std::size_t>(frame.stillChunkSlots, 1);
    CameraCullFrame result;

    if (packedVersion != frame.stillVersion || records.size() != stillCount || chunkSlots != frameChunkSlots || (stillCount == 0 && generation == 0)) {
        const bool follows = packedVersion != 0 && packedVersion == frame.stillPreviousVersion && records.size() <= stillCount && stillCount > 0 &&
                             chunkSlots == frameChunkSlots;

        if (!follows || !repackRanges(frame, placement, result)) {
            buildStill(frame, placement, result);
        }

        packedVersion = frame.stillVersion;
        ++generation;
    }

    const std::size_t movingCount = frame.draws.size() - stillCount;
    result.stillRecords = records;
    result.stillChunks = chunks;
    result.chunkSlots = chunkSlots;
    result.stillCandidates = candidates;
    result.stillSegments = segments;
    result.stillGeneration = generation;
    result.movingRecords.reserve(movingCount);

    for (std::size_t draw = stillCount; draw < frame.draws.size(); ++draw) {
        result.movingRecords.push_back(packCullRecord(frame.draws[draw]));
    }

    CullGroups moving = buildCullGroups(frame.draws, stillCount, movingCount, placement);
    const std::uint32_t candidateOffset = static_cast<std::uint32_t>(candidates.size());
    const std::uint32_t segmentOffset = static_cast<std::uint32_t>(segments.size());

    for (GpuCullSegment& segment : moving.segments) {
        segment.firstCandidate += candidateOffset;
    }

    for (GpuCullGroup& group : moving.groups) {
        group.firstSegment += segmentOffset;
    }

    result.movingCandidates = std::move(moving.candidates);
    result.movingSegments = std::move(moving.segments);
    result.groups = groups;
    result.groups.insert(result.groups.end(), moving.groups.begin(), moving.groups.end());
    result.styles = styles;
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

std::size_t CameraCullTable::getChunkBuilds() const {
    return chunkBuilds;
}

void CameraCullTable::buildStill(const FrameDescription& frame, const std::function<MeshPlacement(MeshHandle)>& placement, CameraCullFrame& result) {
    const std::size_t stillCount = std::min(frame.stillDraws, frame.draws.size());
    chunkSlots = std::max<std::size_t>(frame.stillChunkSlots, 1);
    records.clear();
    keys.clear();
    records.reserve(stillCount);
    keys.reserve(stillCount);

    for (std::size_t draw = 0; draw < stillCount; ++draw) {
        records.push_back(packCullRecord(frame.draws[draw]));
        keys.push_back(candidateKey(frame.draws[draw]));
    }

    const std::size_t chunkCount = (stillCount + chunkSlots - 1) / chunkSlots;
    chunks.assign(chunkCount, GpuCullChunk{});
    chunkCandidates.assign(chunkCount, ChunkCandidates{});
    candidates.clear();
    candidateRuns.clear();

    for (std::size_t chunk = 0; chunk < chunkCount; ++chunk) {
        const std::size_t first = chunk * chunkSlots;
        chunks[chunk] = packCullChunk(std::span<const GpuCullRecord>{records}.subspan(first, std::min(chunkSlots, stillCount - first)));
        buildChunkCandidates(frame, chunk);
    }

    buildSegments(placement);
    stillLevels.clear();
    tableLevels(frame.draws, 0, stillCount, stillLevels);
    stillGlass.clear();
    stillFlatFaced.clear();
    listGlassAndFlat(frame.draws, 0, stillCount, stillGlass, stillFlatFaced);
    ++stillBuilds;

    result.stillRebuilt = true;
    result.stillChanges.clear();
    result.stillRepacked = {DrawRange{0, stillCount}};
    result.chunksRepacked = {DrawRange{0, chunkCount}};
    result.candidatesRepacked = {DrawRange{0, candidates.size()}};
    result.segmentsChanged = true;
}

/*
* Each built draw's record packed again (its old and new records into the changes; a new slot's new one only), each
* chunk it is in packed again, and the candidates of chunks where a draw's key changed built again, then the segments
* and groups. False (build everything) when a range misses a new slot or reaches past the still draws, or when a mesh's
* levels' meshes changed (every chunk drawing that mesh would change).
*/
bool CameraCullTable::repackRanges(const FrameDescription& frame, const std::function<MeshPlacement(MeshHandle)>& placement, CameraCullFrame& result) {
    const std::size_t stillCount = std::min(frame.stillDraws, frame.draws.size());
    const std::size_t had = records.size();
    const std::vector<DrawRange> ranges = joinDrawRanges(frame.builtStillDraws);
    std::size_t newCovered = 0;

    for (const DrawRange& range : ranges) {
        if (range.first + range.count > stillCount) {
            return false;
        }

        const std::size_t from = std::max(range.first, had);
        newCovered += range.first + range.count > from ? range.first + range.count - from : 0;
    }

    if (newCovered != stillCount - had) {
        return false;
    }

    // The meshes' levels first: a changed level set means a full build, before anything is touched.
    for (const DrawRange& range : ranges) {
        for (std::size_t draw = range.first; draw < range.first + range.count; ++draw) {
            const DrawItem& item = frame.draws[draw];

            if (item.mesh.isValid() && item.mesh.index < stillLevels.size() && stillLevels[item.mesh.index].meshes[0] == item.mesh.index &&
                !sameMeshes(packMeshLevels(item.mesh, item.levels), stillLevels[item.mesh.index])) {
                return false;
            }
        }
    }

    records.resize(stillCount);
    keys.resize(stillCount, ~std::uint64_t{0});
    const std::size_t chunkCount = (stillCount + chunkSlots - 1) / chunkSlots;
    chunks.resize(chunkCount);
    chunkCandidates.resize(chunkCount);
    std::set<std::size_t> touchedChunks;
    std::set<std::size_t> rekeyedChunks;
    bool listsChanged = false;

    for (const DrawRange& range : ranges) {
        for (std::size_t draw = range.first; draw < range.first + range.count; ++draw) {
            const DrawItem& item = frame.draws[draw];

            if (item.mesh.isValid()) {
                if (item.mesh.index >= stillLevels.size()) {
                    stillLevels.resize(item.mesh.index + 1);
                }

                stillLevels[item.mesh.index] = packMeshLevels(item.mesh, item.levels);
            }

            if (draw < had) {
                result.stillChanges.push_back(records[draw]);
            }

            records[draw] = packCullRecord(item);
            result.stillChanges.push_back(records[draw]);
            const std::uint64_t key = candidateKey(item);

            if (draw >= had || key != keys[draw]) {
                rekeyedChunks.insert(draw / chunkSlots);
                listsChanged = true;
                keys[draw] = key;
            }

            touchedChunks.insert(draw / chunkSlots);
        }
    }

    for (const std::size_t chunk : touchedChunks) {
        const std::size_t first = chunk * chunkSlots;
        chunks[chunk] = packCullChunk(std::span<const GpuCullRecord>{records}.subspan(first, std::min(chunkSlots, stillCount - first)));
    }

    std::vector<DrawRange> candidateRanges;

    for (const std::size_t chunk : rekeyedChunks) {
        buildChunkCandidates(frame, chunk);
        candidateRanges.push_back(DrawRange{chunkCandidates[chunk].first, chunkCandidates[chunk].count});
    }

    if (!rekeyedChunks.empty()) {
        buildSegments(placement);
    }

    // The glass and flat-faced lists: the ranges' entries dropped and listed again, kept in draw order.
    if (listsChanged) {
        const auto inRanges = [&ranges](std::size_t draw) {
            return std::any_of(ranges.begin(), ranges.end(), [draw](const DrawRange& range) { return draw >= range.first && draw < range.first + range.count; });
        };
        std::erase_if(stillGlass, inRanges);
        std::erase_if(stillFlatFaced, inRanges);

        for (const DrawRange& range : ranges) {
            listGlassAndFlat(frame.draws, range.first, range.count, stillGlass, stillFlatFaced);
        }

        std::sort(stillGlass.begin(), stillGlass.end());
        std::sort(stillFlatFaced.begin(), stillFlatFaced.end());
    }

    result.stillRepacked = ranges;
    result.chunksRepacked = rangesOf(touchedChunks);
    result.candidatesRepacked = joinDrawRanges(std::move(candidateRanges));
    result.segmentsChanged = !rekeyedChunks.empty();
    return true;
}

/*
* A chunk's candidates, as buildCullGroups makes them (every draw not glass, each slot with a mesh) sorted by group then
* draw, into the chunk's run (a new one, with spare room, when they no longer fit), and its segments: one a run of a key.
*/
void CameraCullTable::buildChunkCandidates(const FrameDescription& frame, std::size_t chunk) {
    struct Keyed {
        std::uint64_t key = 0;
        GpuCullCandidate candidate;
    };

    const std::size_t first = chunk * chunkSlots;
    const std::size_t end = std::min(first + chunkSlots, records.size());
    std::vector<Keyed> keyed;

    for (std::size_t draw = first; draw < end; ++draw) {
        const DrawItem& item = frame.draws[draw];

        if (isTransparent(item.material) || !item.mesh.isValid()) {
            continue;
        }

        if (item.levels.size() >= meshLevelSlots) {
            throw std::invalid_argument("A mesh has at most three levels besides itself");
        }

        const int style = drawStyle(item.material);
        const std::uint32_t index = static_cast<std::uint32_t>(draw);
        keyed.push_back(Keyed{groupKey(style, item.mesh.index), GpuCullCandidate{index, 0}});

        for (std::size_t level = 0; level < item.levels.size(); ++level) {
            if (item.levels[level].mesh.isValid()) {
                keyed.push_back(Keyed{groupKey(style, item.levels[level].mesh.index), GpuCullCandidate{index, static_cast<std::uint32_t>(level + 1)}});
            }
        }
    }

    std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) { return a.key < b.key; });
    ChunkCandidates& run = chunkCandidates[chunk];

    if (keyed.size() > run.room) {
        if (run.room > 0) {
            candidateRuns.giveBack(run.first, run.room);
        }

        run.room = candidateRoom(keyed.size());
        run.first = candidateRuns.take(run.room);

        if (candidates.size() < candidateRuns.end()) {
            candidates.resize(candidateRuns.end());
        }
    }

    run.count = keyed.size();
    run.segments.clear();

    for (std::size_t position = 0; position < keyed.size(); ++position) {
        candidates[run.first + position] = keyed[position].candidate;

        if (position == 0 || keyed[position].key != keyed[position - 1].key) {
            run.segments.emplace_back(keyed[position].key, GpuCullSegment{static_cast<std::uint32_t>(run.first + position), 0, static_cast<std::uint32_t>(chunk), 0});
        }

        ++run.segments.back().second.candidates;
    }

    ++chunkBuilds;
}

/*
* Every chunk's segments gathered in chunk order and sorted by key (stable, so each key's stay in chunk order): the
* segment table, and a group a key, in batchByMesh's order.
*/
void CameraCullTable::buildSegments(const std::function<MeshPlacement(MeshHandle)>& placement) {
    std::vector<std::pair<std::uint64_t, GpuCullSegment>> gathered;

    for (const ChunkCandidates& run : chunkCandidates) {
        gathered.insert(gathered.end(), run.segments.begin(), run.segments.end());
    }

    std::stable_sort(gathered.begin(), gathered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    segments.clear();
    groups.clear();
    styles.clear();

    for (std::size_t position = 0; position < gathered.size(); ++position) {
        const std::uint64_t key = gathered[position].first;

        if (position == 0 || key != gathered[position - 1].first) {
            const int style = static_cast<int>(key >> 32u);
            const MeshHandle mesh{static_cast<std::uint32_t>(key & 0xFFFFFFFFu)};
            const MeshPlacement placed = placement(mesh);
            GpuCullGroup group;
            group.firstSegment = static_cast<std::uint32_t>(segments.size());
            group.indexCount = placed.indexCount;
            group.firstIndex = placed.firstIndex;
            group.vertexOffset = static_cast<std::int32_t>(placed.firstVertex);
            groups.push_back(group);
            styles.push_back(CullGroupStyle{mesh, (style & 2) != 0, (style & 1) != 0, placed.pooled});
        }

        ++groups.back().segments;
        segments.push_back(gathered[position].second);
    }
}
