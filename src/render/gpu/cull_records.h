#pragma once

#include "math/frustum.h"
#include "math/vec3.h"
#include "render/gpu/cull_records_shared.h"
#include "scene/frame_description.h"
#include "scene/level_of_detail.h"
#include "scene/mesh_level.h"

#include <cstdint>
#include <span>

/*
* GPU-driven drawing's records (docs/research/GPUDRIVEN.md, stage 2): what a culling thread needs of each draw, each
* mesh's levels and each view, packed for the GPU, and the choices made from them, mirrored by culling.glsl and
* level_of_detail.glsl. The CPU's own choices (scene/culling, scene/level_of_detail, math/frustum) are the reference:
* tests check that choosing from the packed records gives the same answers. Nothing reads them on the GPU yet; stage 3
* uploads them with the pass that does.
*/

static_assert(CULL_LEVEL_SLOTS == meshLevelSlots, "The GPU's level slots must match scene/mesh_level.h");

inline constexpr std::uint32_t cullNoMesh = CULL_NO_MESH;
inline constexpr int cullNoSlot = CULL_NO_SLOT;

// One draw for a culling thread: 48 bytes beside its 208-byte instance (the same index). Must match culling.glsl's.
struct GpuCullRecord {
    float bounds[4];      // its bounding sphere in the world: xyz centre, w radius (below 0 unknown: never culled)
    float levelSphere[4]; // the sphere its level is chosen by (DrawItem::levelBounds, else bounds)
    std::uint32_t flags = 0; // CULL_* bits
    std::uint32_t mesh = 0;  // its mesh's number, which also names its level table
    float hiddenBelowError = 0.0f; // an impostor's: drawn only where this error (metres) fits (DrawItem::hiddenBelowError)
    std::int32_t shadowCascades = 0; // the sun's nearest cascades it casts into (DrawItem::shadowCascades; 0 all)
};

static_assert(sizeof(GpuCullRecord) == 48, "GpuCullRecord must match culling.glsl's CullRecord");

// A mesh's levels: slot 0 the mesh itself, then its levels coarsest last (cullNoMesh: one drawing nothing).
struct GpuMeshLevels {
    std::uint32_t meshes[CULL_LEVEL_SLOTS];
    float errorMetres[CULL_LEVEL_SLOTS];  // each slot's error (slot 0's unused)
    std::uint32_t levels = 0;             // levels after the mesh itself (0 to 3)
    std::uint32_t sharesVertices = 0;     // bit k: slot k uses the mesh's first vertices, so card thinning shapes it (bit 0 always)
    std::uint32_t changesShape = 0;       // 1: the mesh deformed this frame (cloth), so the velocity pass redraws its draws
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuMeshLevels) == 48, "GpuMeshLevels must match level_of_detail.glsl's MeshLevels");

// A view's level choice (LevelView): eye in the world and pixels a metre away (xyz, w), the pixel error, CULL_LEVELS_* flags,
// the fade band (0 none).
struct GpuLevelView {
    float eye[4];
    float pixelError = 1.0f;
    std::uint32_t flags = 0;
    float fadeBand = 0.0f;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuLevelView) == 32, "GpuLevelView must match level_of_detail.glsl's LevelView");

// A view's six planes (Frustum: inward normal xyz, distance w).
struct GpuFrustum {
    float planes[6][4];
};

static_assert(sizeof(GpuFrustum) == 96, "GpuFrustum must match culling.glsl's");

GpuCullRecord packCullRecord(const DrawItem& draw);

// The flag bits naming the nearest sun cascade a caster casts into (held to CULL_SHADOW_FROM_MASK), and back. Mirrored
// by culling.glsl.
std::uint32_t cullShadowFromBits(int cascade);
int cullShadowFrom(std::uint32_t flags);

inline constexpr std::uint32_t cullNoChunk = CULL_NO_CHUNK;

/*
* A chunk of still draws (GPU-driven drawing's stage 7b: slots slot / chunkSlots, scene/still_draws.h), tested once a
* view before its draws: the box round its members' spheres, grown by the margin, and CULL_CHUNK_* flags. Must match
* camera_culling.glsl's CullChunk.
*/
struct GpuCullChunk {
    float minimum[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t flags = 0;
    float maximum[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuCullChunk) == 32, "GpuCullChunk must match camera_culling.glsl's CullChunk");

// The chunk of these draws' records: draws with a mesh and not transparent (the only candidates) count; empty slots and
// glass are left out. None counting: no flags (no view takes it).
GpuCullChunk packCullChunk(std::span<const GpuCullRecord> members);

// A box (low to high) as a chunk's, grown by the chunks' margin (CULL_CHUNK_MARGIN and a share of its farthest
// coordinate), which outgrows the GPU's and CPU's rounding; flags kept.
void setCullChunkBox(GpuCullChunk& chunk, const float low[3], const float high[3]);
GpuMeshLevels packMeshLevels(MeshHandle mesh, std::span<const MeshLevel> levels);
GpuLevelView packLevelView(const LevelView& view);
GpuFrustum packFrustum(const Frustum& frustum);

// ---- Mirrored by culling.glsl ----

// Whether a sphere (xyz centre, w radius) may be inside all six planes; unknown bounds (w below 0) always may
// (Frustum::intersectsSphere, as the CPU's culling calls it).
bool sphereInFrustum(const GpuFrustum& frustum, const float sphere[4]);

// The camera's opaque views take a draw that is neither shadow-only, hidden nor transparent, and may be in view
// (visibleDraws, then opaqueDraws).
bool cameraTakes(const GpuCullRecord& record, const GpuFrustum& frustum);

// ---- Mirrored by level_of_detail.glsl ----

// errorInPixels, from packed numbers.
float levelErrorInPixels(float errorMetres, float distance, float pixelsPerMetre, bool orthographic);

// levelDistance: to the sphere's surface, 0 from inside.
float levelSphereDistance(const float sphere[4], const Vec3& eye);

// The slot a draw is drawn with in the view (chosenMesh): 0 the mesh, k its k-th level; cullNoSlot where it draws
// nothing (an impostor not yet far enough, a tree past its last level).
int chooseLevelSlot(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuLevelView& view);

// The slot the levels' walk ends on under a budget of pixels, which may hold no mesh; -1 for an impostor not drawn there.
int levelSlotWithin(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuLevelView& view, float budget);

// A draw crossing a switch in the view's fade band (levelFade): its finer and coarser slots (cullNoSlot: none drawn)
// and the share of the band crossed.
struct GpuLevelFade {
    bool fading = false;
    int finer = cullNoSlot;
    int coarser = cullNoSlot;
    float share = 0.0f;
};

GpuLevelFade levelFadeSlots(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuLevelView& view);

// Whether that slot is drawn with its cards thinned (DrawItem::thinningEntry, and the slot sharing the mesh's vertices).
bool slotThinned(const GpuCullRecord& record, const GpuMeshLevels& levels, int slot);

// Whether a draw's choice in a view is within rounding of changing: a level's (or an impostor's) error within 1e-4 of
// the pixel budget (or, fading, of the band's wider one), a fade's share within 1e-3 of a step of its field, or its
// sphere within a millimetre of a plane, where the GPU's square root may round either way.
bool nearAChoiceEdge(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuLevelView& view, const GpuFrustum& frustum);
