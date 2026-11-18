// GPU-driven drawing's level choice, included with #include "level_of_detail.glsl" after culling.glsl: mirrors
// render/gpu/cull_records.h (levelErrorInPixels, levelSphereDistance, levelSlotWithin, chooseLevelSlot, levelFadeSlots,
// slotThinned), which equal the CPU's scene/level_of_detail. Pure functions.
#ifndef LEVEL_OF_DETAIL_GLSL
#define LEVEL_OF_DETAIL_GLSL

#include "render/gpu/cull_records_shared.h"
#include "culling.glsl"

// A mesh's levels: slot 0 the mesh itself, then its levels coarsest last. Must match GpuMeshLevels.
struct MeshLevels {
    uint meshes[CULL_LEVEL_SLOTS];  // CULL_NO_MESH: a slot drawing nothing
    float errorMetres[CULL_LEVEL_SLOTS];
    uint levels;                    // levels after the mesh itself
    uint sharesVertices;            // bit k: slot k uses the mesh's first vertices
    uint changesShape;              // 1: the mesh deformed this frame
    uint unused;
};

// A view's level choice. Must match GpuLevelView.
struct LevelView {
    vec4 eye;          // xyz the eye in the world, w pixels a metre away
    float pixelError;
    uint flags;        // CULL_LEVELS_*
    float fadeBand;    // the share of a switch's distance its fading draws cross-fade over; 0 none
    uint unused;
};

// How many pixels an error covers at a distance; infinite at 0 or nearer; distance-free in an orthographic view.
float levelErrorInPixels(float errorMetres, float distance, float pixelsPerMetre, bool orthographic) {
    if (orthographic) {
        return errorMetres * pixelsPerMetre;
    }

    if (distance <= 0.0) {
        return uintBitsToFloat(0x7f800000u); // infinity
    }

    return errorMetres * pixelsPerMetre / distance;
}

// To the sphere's surface, 0 from inside.
float levelSphereDistance(vec4 sphere, vec3 eye) {
    return max(length(sphere.xyz - eye) - sphere.w, 0.0);
}

// The slot the levels' walk ends on under a budget of pixels, which may hold no mesh; -1 for an impostor not drawn there
// (levelSlotWithin).
int levelSlotWithin(CullRecord record, MeshLevels levels, LevelView view, float budget) {
    bool enabled = (view.flags & CULL_LEVELS_ENABLED) != 0u;
    bool impostors = (view.flags & CULL_LEVELS_IMPOSTORS) != 0u;
    bool orthographic = (view.flags & CULL_LEVELS_ORTHOGRAPHIC) != 0u;

    if (record.hiddenBelowError > 0.0) {
        if (!enabled || !impostors || record.levelSphere.w < 0.0 ||
            levelErrorInPixels(record.hiddenBelowError, levelSphereDistance(record.levelSphere, view.eye.xyz), view.eye.w, orthographic) > budget) {
            return -1;
        }
    }

    uint chosen = 0u;

    if (enabled && levels.levels > 0u && record.levelSphere.w >= 0.0) {
        float distance = levelSphereDistance(record.levelSphere, view.eye.xyz);

        for (uint level = 0u; level < levels.levels; ++level) {
            uint slot = level + 1u;

            if (levelErrorInPixels(levels.errorMetres[slot], distance, view.eye.w, orthographic) > budget ||
                (!impostors && levels.meshes[slot] == CULL_NO_MESH)) {
                break;
            }

            chosen = slot;
        }
    }

    return int(chosen);
}

// A slot from the walk as drawn: none for an impostor not drawn or a slot without a mesh.
int drawnLevelSlot(MeshLevels levels, int slot) {
    return slot < 0 || levels.meshes[slot] == CULL_NO_MESH ? CULL_NO_SLOT : slot;
}

// The slot a draw is drawn with in the view: 0 the mesh, k its k-th level, CULL_NO_SLOT for nothing (chosenMesh).
int chooseLevelSlot(CullRecord record, MeshLevels levels, LevelView view) {
    return drawnLevelSlot(levels, levelSlotWithin(record, levels, view, view.pixelError));
}

// A draw crossing a switch in the view's fade band (levelFadeSlots): its finer and coarser slots (CULL_NO_SLOT: none
// drawn) and the share of the band crossed. Must match GpuLevelFade.
struct LevelFade {
    bool fading;
    int finer;
    int coarser;
    float share;
};

// levelFade: the walk under the budget and under the band's wider one, the share by the coarser's error.
LevelFade levelFadeSlots(CullRecord record, MeshLevels levels, LevelView view) {
    LevelFade none = LevelFade(false, CULL_NO_SLOT, CULL_NO_SLOT, 0.0);
    bool enabled = (view.flags & CULL_LEVELS_ENABLED) != 0u;
    bool orthographic = (view.flags & CULL_LEVELS_ORTHOGRAPHIC) != 0u;

    if ((record.flags & CULL_FADES) == 0u || !(view.fadeBand > 0.0) || orthographic || !enabled) {
        return none;
    }

    float keep = 1.0 - view.fadeBand;
    int finer = levelSlotWithin(record, levels, view, view.pixelError);
    int coarser = levelSlotWithin(record, levels, view, view.pixelError / keep);

    if (finer == coarser) {
        return none;
    }

    float error = finer < 0 ? record.hiddenBelowError : levels.errorMetres[coarser];
    float pixels = levelErrorInPixels(error, levelSphereDistance(record.levelSphere, view.eye.xyz), view.eye.w, false);
    float share = clamp((view.pixelError / pixels - keep) / view.fadeBand, 0.0, 1.0);
    return LevelFade(true, drawnLevelSlot(levels, finer), drawnLevelSlot(levels, coarser), share);
}

// Whether that slot is drawn with its cards thinned.
bool slotThinned(CullRecord record, MeshLevels levels, int slot) {
    return slot >= 0 && (record.flags & CULL_THINNABLE) != 0u && (levels.sharesVertices & (1u << uint(slot))) != 0u;
}

#endif
