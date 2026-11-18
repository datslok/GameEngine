#include "render/gpu/cull_records.h"
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
                   (draw.sway.kind != SwayKind::Still ? CULL_SWAYS : 0u);
    record.mesh = draw.mesh.index;
    record.hiddenBelowError = draw.hiddenBelowError;
    return record;
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
* chosenMesh, step for step: an impostor is drawn only where its own error fits (never with levels or impostors off);
* then the levels walk coarser until one does not fit, or (impostors off) one draws nothing; the slot chosen may still
* hold no mesh (a tree past its last level).
*/
int chooseLevelSlot(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuLevelView& view) {
    const bool enabled = (view.flags & CULL_LEVELS_ENABLED) != 0u;
    const bool impostors = (view.flags & CULL_LEVELS_IMPOSTORS) != 0u;
    const bool orthographic = (view.flags & CULL_LEVELS_ORTHOGRAPHIC) != 0u;
    const Vec3 eye{view.eye[0], view.eye[1], view.eye[2]};
    const float pixelsPerMetre = view.eye[3];

    if (record.hiddenBelowError > 0.0f) {
        if (!enabled || !impostors || record.levelSphere[3] < 0.0f ||
            levelErrorInPixels(record.hiddenBelowError, levelSphereDistance(record.levelSphere, eye), pixelsPerMetre, orthographic) > view.pixelError) {
            return cullNoSlot;
        }
    }

    std::uint32_t chosen = 0;

    if (enabled && levels.levels > 0 && record.levelSphere[3] >= 0.0f) {
        const float distance = levelSphereDistance(record.levelSphere, eye);

        for (std::uint32_t level = 0; level < levels.levels; ++level) {
            const std::uint32_t slot = level + 1;

            if (levelErrorInPixels(levels.errorMetres[slot], distance, pixelsPerMetre, orthographic) > view.pixelError ||
                (!impostors && levels.meshes[slot] == cullNoMesh)) {
                break;
            }

            chosen = slot;
        }
    }

    return levels.meshes[chosen] == cullNoMesh ? cullNoSlot : static_cast<int>(chosen);
}

bool slotThinned(const GpuCullRecord& record, const GpuMeshLevels& levels, int slot) {
    return slot >= 0 && (record.flags & CULL_THINNABLE) != 0u && (levels.sharesVertices & (1u << slot)) != 0u;
}

bool nearAChoiceEdge(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuLevelView& view, const GpuFrustum& frustum) {
    const Vec3 eye{view.eye[0], view.eye[1], view.eye[2]};
    const bool orthographic = (view.flags & CULL_LEVELS_ORTHOGRAPHIC) != 0u;
    const float distance = levelSphereDistance(record.levelSphere, eye);
    const auto near = [&](float errorMetres) {
        const float ratio = levelErrorInPixels(errorMetres, distance, view.eye[3], orthographic) / view.pixelError;
        return std::abs(ratio - 1.0f) < 1e-4f;
    };

    bool any = record.hiddenBelowError > 0.0f && near(record.hiddenBelowError);

    for (std::uint32_t slot = 1; slot <= levels.levels; ++slot) {
        any = any || near(levels.errorMetres[slot]);
    }

    for (const auto& plane : frustum.planes) {
        const float signedDistance = plane[0] * record.bounds[0] + plane[1] * record.bounds[1] + plane[2] * record.bounds[2] + plane[3];
        any = any || std::abs(signedDistance + record.bounds[3]) < 1e-3f;
    }

    return any;
}
