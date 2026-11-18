// GPU-driven drawing's level choice, included with #include "level_of_detail.glsl" after culling.glsl: mirrors
// render/gpu/cull_records.h (levelErrorInPixels, levelSphereDistance, chooseLevelSlot, slotThinned), which equal the
// CPU's scene/level_of_detail. Pure functions.
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
    uint unused0;
    uint unused1;
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

// The slot a draw is drawn with in the view: 0 the mesh, k its k-th level, CULL_NO_SLOT for nothing (chosenMesh).
int chooseLevelSlot(CullRecord record, MeshLevels levels, LevelView view) {
    bool enabled = (view.flags & CULL_LEVELS_ENABLED) != 0u;
    bool impostors = (view.flags & CULL_LEVELS_IMPOSTORS) != 0u;
    bool orthographic = (view.flags & CULL_LEVELS_ORTHOGRAPHIC) != 0u;

    if (record.hiddenBelowError > 0.0) {
        if (!enabled || !impostors || record.levelSphere.w < 0.0 ||
            levelErrorInPixels(record.hiddenBelowError, levelSphereDistance(record.levelSphere, view.eye.xyz), view.eye.w, orthographic) > view.pixelError) {
            return CULL_NO_SLOT;
        }
    }

    uint chosen = 0u;

    if (enabled && levels.levels > 0u && record.levelSphere.w >= 0.0) {
        float distance = levelSphereDistance(record.levelSphere, view.eye.xyz);

        for (uint level = 0u; level < levels.levels; ++level) {
            uint slot = level + 1u;

            if (levelErrorInPixels(levels.errorMetres[slot], distance, view.eye.w, orthographic) > view.pixelError ||
                (!impostors && levels.meshes[slot] == CULL_NO_MESH)) {
                break;
            }

            chosen = slot;
        }
    }

    return levels.meshes[chosen] == CULL_NO_MESH ? CULL_NO_SLOT : int(chosen);
}

// Whether that slot is drawn with its cards thinned.
bool slotThinned(CullRecord record, MeshLevels levels, int slot) {
    return slot >= 0 && (record.flags & CULL_THINNABLE) != 0u && (levels.sharesVertices & (1u << uint(slot))) != 0u;
}

#endif
