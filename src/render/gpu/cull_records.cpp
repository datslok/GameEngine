#include "render/gpu/cull_records.h"
#include "render/gpu/level_fade.h"
#include "scene/material.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
    void writeSphere(float* target, const BoundingSphere& sphere) {
        target[0] = sphere.centre.x;
        target[1] = sphere.centre.y;
        target[2] = sphere.centre.z;
        target[3] = sphere.radius;
    }
}

GpuCullRecord packCullRecord(const DrawItem& draw) {
    GpuCullRecord record{};
    writeSphere(record.bounds, draw.bounds);
    writeSphere(record.levelSphere, draw.levelBounds.radius >= 0.0f ? draw.levelBounds : draw.bounds);
    record.flags = (draw.shadowOnly ? CULL_SHADOW_ONLY : 0u) | (draw.hidden ? CULL_HIDDEN : 0u) |
                   (isTransparent(draw.material) ? CULL_TRANSPARENT : 0u) | (draw.castsShadows ? CULL_CASTS_SHADOWS : 0u) |
                   (isCutOut(draw.material) ? CULL_CUT_OUT : 0u) | (draw.material.doubleSided ? CULL_DOUBLE_SIDED : 0u) |
                   (draw.thinningEntry != 0 ? CULL_THINNABLE : 0u) |
                   (draw.previousModel && *draw.previousModel != draw.model ? CULL_MOVED : 0u) |
                   (draw.sway.kind != SwayKind::Still ? CULL_SWAYS : 0u) | (draw.fadesLevels ? CULL_FADES : 0u);
    record.mesh = draw.mesh.index;
    record.hiddenBelowError = draw.hiddenBelowError;
    record.shadowCascades = draw.shadowCascades;
    return record;
}

/*
* The members' flags gathered, their spheres' box, then the margin: CULL_CHUNK_MARGIN metres and a share of the farthest
* coordinate, more than the GPU's and CPU's rounding can move a plane test, so a chunk culled never held a draw kept.
*/
GpuCullChunk packCullChunk(std::span<const GpuCullRecord> members) {
    GpuCullChunk chunk{};
    float low[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    float high[3] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    bool anyBox = false;

    for (const GpuCullRecord& record : members) {
        if (record.mesh == cullNoMesh || (record.flags & CULL_TRANSPARENT) != 0u) {
            continue;
        }

        if ((record.flags & (CULL_SHADOW_ONLY | CULL_HIDDEN)) == 0u) {
            chunk.flags |= CULL_CHUNK_SEEN;
        }

        if ((record.flags & CULL_CASTS_SHADOWS) != 0u && (record.flags & CULL_HIDDEN) == 0u) {
            chunk.flags |= CULL_CHUNK_CASTS;
        }

        if (record.bounds[3] < 0.0f) {
            chunk.flags |= CULL_CHUNK_ALWAYS;
            continue;
        }

        for (int axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(low[axis], record.bounds[axis] - record.bounds[3]);
            high[axis] = std::max(high[axis], record.bounds[axis] + record.bounds[3]);
        }

        anyBox = true;
    }

    if (!anyBox) {
        return chunk;
    }

    float farthest = 0.0f;

    for (int axis = 0; axis < 3; ++axis) {
        farthest = std::max({farthest, std::abs(low[axis]), std::abs(high[axis])});
    }

    const float margin = static_cast<float>(CULL_CHUNK_MARGIN) + static_cast<float>(CULL_CHUNK_RELATIVE_MARGIN) * farthest;

    for (int axis = 0; axis < 3; ++axis) {
        chunk.minimum[axis] = low[axis] - margin;
        chunk.maximum[axis] = high[axis] + margin;
    }

    return chunk;
}

// Slot 0 is the mesh itself, always sharing its own vertices; the rest follow the levels, coarsest last.
GpuMeshLevels packMeshLevels(MeshHandle mesh, std::span<const MeshLevel> levels) {
    if (levels.size() >= CULL_LEVEL_SLOTS) {
        throw std::invalid_argument("A mesh has at most three levels besides itself");
    }

    GpuMeshLevels packed{};
    std::fill(std::begin(packed.meshes), std::end(packed.meshes), cullNoMesh);
    packed.meshes[0] = mesh.index;
    packed.sharesVertices = 1u;
    packed.levels = static_cast<std::uint32_t>(levels.size());

    for (std::size_t level = 0; level < levels.size(); ++level) {
        packed.meshes[level + 1] = levels[level].mesh.isValid() ? levels[level].mesh.index : cullNoMesh;
        packed.errorMetres[level + 1] = levels[level].errorMetres;
        packed.sharesVertices |= levels[level].sharesVertices ? 1u << (level + 1) : 0u;
    }

    return packed;
}

GpuLevelView packLevelView(const LevelView& view) {
    GpuLevelView packed{};
    packed.eye[0] = view.eye.x;
    packed.eye[1] = view.eye.y;
    packed.eye[2] = view.eye.z;
    packed.eye[3] = view.pixelsPerMetre;
    packed.pixelError = view.pixelError;
    packed.flags = (view.enabled ? CULL_LEVELS_ENABLED : 0u) | (view.impostors ? CULL_LEVELS_IMPOSTORS : 0u) |
                   (view.orthographic ? CULL_LEVELS_ORTHOGRAPHIC : 0u);
    packed.fadeBand = view.fadeBand;
    return packed;
}

GpuFrustum packFrustum(const Frustum& frustum) {
    GpuFrustum packed{};

    for (int plane = 0; plane < 6; ++plane) {
        packed.planes[plane][0] = frustum.planes[plane].normal.x;
        packed.planes[plane][1] = frustum.planes[plane].normal.y;
        packed.planes[plane][2] = frustum.planes[plane].normal.z;
        packed.planes[plane][3] = frustum.planes[plane].distance;
    }

    return packed;
}

// Each plane's signed distance as Plane::signedDistance sums it (normal · centre, then the distance).
bool sphereInFrustum(const GpuFrustum& frustum, const float sphere[4]) {
    if (sphere[3] < 0.0f) {
        return true;
    }

    for (const auto& plane : frustum.planes) {
        const float signedDistance = plane[0] * sphere[0] + plane[1] * sphere[1] + plane[2] * sphere[2] + plane[3];

        if (signedDistance < -sphere[3]) {
            return false;
        }
    }

    return true;
}

bool cameraTakes(const GpuCullRecord& record, const GpuFrustum& frustum) {
    return (record.flags & (CULL_SHADOW_ONLY | CULL_HIDDEN | CULL_TRANSPARENT)) == 0u && sphereInFrustum(frustum, record.bounds);
}

float levelErrorInPixels(float errorMetres, float distance, float pixelsPerMetre, bool orthographic) {
    if (orthographic) {
        return errorMetres * pixelsPerMetre;
    }

    if (distance <= 0.0f) {
        return std::numeric_limits<float>::infinity();
    }

    return errorMetres * pixelsPerMetre / distance;
}

float levelSphereDistance(const float sphere[4], const Vec3& eye) {
    const Vec3 offset{sphere[0] - eye.x, sphere[1] - eye.y, sphere[2] - eye.z};
    return std::max(offset.length() - sphere[3], 0.0f);
}

/*
* chosenMesh's walk, step for step, under a budget: an impostor is drawn only where its own error fits (never with
* levels or impostors off); then the levels walk coarser until one does not fit, or (impostors off) one draws nothing;
* the slot it ends on may hold no mesh (a tree past its last level).
*/
int levelSlotWithin(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuLevelView& view, float budget) {
    const bool enabled = (view.flags & CULL_LEVELS_ENABLED) != 0u;
    const bool impostors = (view.flags & CULL_LEVELS_IMPOSTORS) != 0u;
    const bool orthographic = (view.flags & CULL_LEVELS_ORTHOGRAPHIC) != 0u;
    const Vec3 eye{view.eye[0], view.eye[1], view.eye[2]};
    const float pixelsPerMetre = view.eye[3];

    if (record.hiddenBelowError > 0.0f) {
        if (!enabled || !impostors || record.levelSphere[3] < 0.0f ||
            levelErrorInPixels(record.hiddenBelowError, levelSphereDistance(record.levelSphere, eye), pixelsPerMetre, orthographic) > budget) {
            return -1;
        }
    }

    std::uint32_t chosen = 0;

    if (enabled && levels.levels > 0 && record.levelSphere[3] >= 0.0f) {
        const float distance = levelSphereDistance(record.levelSphere, eye);

        for (std::uint32_t level = 0; level < levels.levels; ++level) {
            const std::uint32_t slot = level + 1;

            if (levelErrorInPixels(levels.errorMetres[slot], distance, pixelsPerMetre, orthographic) > budget ||
                (!impostors && levels.meshes[slot] == cullNoMesh)) {
                break;
            }

            chosen = slot;
        }
    }

    return static_cast<int>(chosen);
}

namespace {
    // A slot from the walk as drawn: none for an impostor not drawn or a slot without a mesh.
    int drawnSlot(const GpuMeshLevels& levels, int slot) {
        return slot < 0 || levels.meshes[slot] == cullNoMesh ? cullNoSlot : slot;
    }
}

int chooseLevelSlot(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuLevelView& view) {
    return drawnSlot(levels, levelSlotWithin(record, levels, view, view.pixelError));
}

// levelFade, step for step: the walk under the budget and under the band's wider one, the share by the coarser's error.
GpuLevelFade levelFadeSlots(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuLevelView& view) {
    const bool enabled = (view.flags & CULL_LEVELS_ENABLED) != 0u;
    const bool orthographic = (view.flags & CULL_LEVELS_ORTHOGRAPHIC) != 0u;

    if ((record.flags & CULL_FADES) == 0u || !(view.fadeBand > 0.0f) || orthographic || !enabled) {
        return GpuLevelFade{};
    }

    const float keep = 1.0f - view.fadeBand;
    const int finer = levelSlotWithin(record, levels, view, view.pixelError);
    const int coarser = levelSlotWithin(record, levels, view, view.pixelError / keep);

    if (finer == coarser) {
        return GpuLevelFade{};
    }

    const Vec3 eye{view.eye[0], view.eye[1], view.eye[2]};
    const float error = finer < 0 ? record.hiddenBelowError : levels.errorMetres[coarser];
    const float pixels = levelErrorInPixels(error, levelSphereDistance(record.levelSphere, eye), view.eye[3], false);
    const float share = std::clamp((view.pixelError / pixels - keep) / view.fadeBand, 0.0f, 1.0f);
    return GpuLevelFade{.fading = true, .finer = drawnSlot(levels, finer), .coarser = drawnSlot(levels, coarser), .share = share};
}

bool slotThinned(const GpuCullRecord& record, const GpuMeshLevels& levels, int slot) {
    return slot >= 0 && (record.flags & CULL_THINNABLE) != 0u && (levels.sharesVertices & (1u << slot)) != 0u;
}

bool nearAChoiceEdge(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuLevelView& view, const GpuFrustum& frustum) {
    const Vec3 eye{view.eye[0], view.eye[1], view.eye[2]};
    const bool orthographic = (view.flags & CULL_LEVELS_ORTHOGRAPHIC) != 0u;
    const float distance = levelSphereDistance(record.levelSphere, eye);
    // A fading draw's band starts where an error covers pixelError / (1 - band).
    const bool fades = (record.flags & CULL_FADES) != 0u && view.fadeBand > 0.0f && !orthographic;
    const auto nearBudget = [&](float errorMetres, float budget) {
        const float ratio = levelErrorInPixels(errorMetres, distance, view.eye[3], orthographic) / budget;
        return std::abs(ratio - 1.0f) < 1e-4f;
    };
    const auto near = [&](float errorMetres) {
        return nearBudget(errorMetres, view.pixelError) || (fades && nearBudget(errorMetres, view.pixelError / (1.0f - view.fadeBand)));
    };

    bool any = record.hiddenBelowError > 0.0f && near(record.hiddenBelowError);

    for (std::uint32_t slot = 1; slot <= levels.levels; ++slot) {
        any = any || near(levels.errorMetres[slot]);
    }

    // A share on a step of the fade's field may round into either.
    const GpuLevelFade fade = levelFadeSlots(record, levels, view);

    if (fade.fading) {
        const float steps = fade.share * static_cast<float>(drawListFadeSteps);
        any = any || std::abs(steps - std::round(steps)) < 1e-3f;
    }

    for (const auto& plane : frustum.planes) {
        const float signedDistance = plane[0] * record.bounds[0] + plane[1] * record.bounds[1] + plane[2] * record.bounds[2] + plane[3];
        any = any || std::abs(signedDistance + record.bounds[3]) < 1e-3f;
    }

    return any;
}
