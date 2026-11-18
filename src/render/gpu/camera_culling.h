#pragma once

#include "render/gpu/camera_culling_shared.h"
#include "render/gpu/cull_records.h"
#include "render/gpu/gpu_scene.h"
#include "scene/frame_description.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

/*
* The camera's lists built on the GPU (docs/research/GPUDRIVEN.md, stage 3; docs/architecture/gpu-driven.md). Every way a
* draw may be drawn, a candidate (the draw and one of its level slots), sits in a group of one mesh drawn one way, the
* groups in batchByMesh's order (style, then mesh) and each group's candidates in draw order. Each frame the GPU flags
* the candidates the camera draws (cameraCandidateFlags), a prefix sum over the flags gives each its place, and each
* writes its draw list entry there and each group its indirect command: batchByMesh's lists, without a sort.
*
* The still draws' groups, records and level tables are kept (CameraCullTable) and built again only when the still
* draws change; the moving draws get their own groups each frame. Render-only and API-free: cullCameraOnCpu runs the
* passes' steps on the CPU, for tests and for cull-compare's reference.
*/

static_assert(CAMERA_CULL_COUNTS >= CULL_LEVEL_SLOTS + 2, "The counts hold every level slot and both totals");

inline constexpr std::uint32_t cameraCullGroupSize = CAMERA_CULL_GROUP_SIZE;

// A draw drawn with one of its meshes. Must match camera_culling.glsl's CullCandidate.
struct GpuCullCandidate {
    std::uint32_t draw = 0;
    std::uint32_t slot = 0;
};

static_assert(sizeof(GpuCullCandidate) == 8, "GpuCullCandidate must match camera_culling.glsl's CullCandidate");

// A group: its run of candidates and its mesh's index count, for its indirect command. Must match camera_cull_write.comp.
struct GpuCullGroup {
    std::uint32_t firstCandidate = 0;
    std::uint32_t candidates = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuCullGroup) == 16, "GpuCullGroup must match camera_cull_write.comp's Group");

// How a group is drawn: its mesh and its pipeline (cut-out, double-sided).
struct CullGroupStyle {
    MeshHandle mesh;
    bool cutOut = false;
    bool doubleSided = false;
};

// The frame's camera, read by every pass. Must match camera_culling.glsl's CameraCull.
struct GpuCameraCull {
    GpuFrustum frustum{};
    GpuLevelView levels{};
    float swayFadeEnd = 0.0f;
    std::uint32_t flags = 0;
    std::uint32_t candidates = 0;
    std::uint32_t groups = 0;
    std::uint32_t mainBase = 0;
    std::uint32_t motionBase = 0;
    std::uint32_t blocks = 0;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuCameraCull) == 160, "GpuCameraCull must match camera_culling.glsl's CameraCull");

// What the passes count (CAMERA_CULL_COUNTS words): draws by level slot, the camera's and the velocity pass's totals.
struct GpuCameraCullCounts {
    std::uint32_t values[CAMERA_CULL_COUNTS] = {};
};

static_assert(sizeof(GpuCameraCullCounts) == 32, "GpuCameraCullCounts must match camera_cull_flags.comp's Counts");

/*
* Groups of draws first to first + count of draws, numbered as draw indices into the whole list: every draw not
* transparent, each mesh slot it has (glass is sorted on the CPU; shadow-only and hidden draws are candidates, so a draw
* hidden or shown keeps its groups, and the GPU drops them by their flags). indexCount gives a mesh's indices.
*/
struct CullGroups {
    std::vector<GpuCullGroup> groups;
    std::vector<CullGroupStyle> styles;
    std::vector<GpuCullCandidate> candidates;
};

CullGroups buildCullGroups(std::span<const DrawItem> draws, std::size_t first, std::size_t count,
                           const std::function<std::uint32_t(MeshHandle)>& indexCount);

/*
* The view's record. motion: the velocity pass's list is built; sway: things sway this frame (the wind's settings on),
* fading out at swayFadeEnd.
*/
GpuCameraCull packCameraCull(const Frustum& frustum, const LevelView& levels, bool motion, bool sway, float swayFadeEnd);

// ---- Mirrored by camera_culling.glsl ----

// Whether part of the sphere is nearer the eye than the fade's end (drawSways).
bool swayReaches(const float bounds[4], const Vec3& eye, float fadeEnd);

// The candidate's CAMERA_CULL_* flags: drawn, moving and thinned, as visibleDraws, chosenMesh and drawSways choose.
std::uint32_t cameraCandidateFlags(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuCameraCull& view, std::uint32_t slot);

// ---- The passes' steps on the CPU ----

// The camera's and the velocity pass's entries (from their bases) and every group's commands, as the GPU writes them.
struct CameraCullResult {
    std::vector<std::uint32_t> mainEntries;
    std::vector<std::uint32_t> motionEntries;
    std::vector<GpuDrawCommand> mainCommands;
    std::vector<GpuDrawCommand> motionCommands;
    std::array<std::uint32_t, CULL_LEVEL_SLOTS> levels{};
};

// The flags, then their prefix sum, then the writes. records by draw, levels by mesh.
CameraCullResult cullCameraOnCpu(std::span<const GpuCullRecord> records, std::span<const GpuMeshLevels> levels, std::span<const GpuCullGroup> groups,
                                 std::span<const GpuCullCandidate> candidates, const GpuCameraCull& view);

/*
* Whether the draw's choice is within rounding of changing (a level's error within 1e-4 of the budget, its sphere
* within a millimetre of a plane or of the sway's reach), where the GPU and the CPU may each be right.
*/
bool nearACameraEdge(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuCameraCull& view);

// ---- cull-compare ----

// One list compared: entries by (mesh, style), every entry with its thinned bit.
struct CullListComparison {
    std::size_t entries = 0;     // the CPU's
    std::size_t matching = 0;    // in both
    std::size_t missing = 0;     // the CPU's, not the GPU's
    std::size_t extra = 0;       // the GPU's, not the CPU's
    std::size_t nearAnEdge = 0;  // of missing and extra, draws near an edge (nearACameraEdge)
};

/*
* The GPU's list (entries read back from base, its groups' commands, styles in group order) against the CPU's
* BatchedDraws, per mesh and way of drawing. nearEdge says whether a draw is near an edge.
*/
CullListComparison compareCullList(std::span<const CullGroupStyle> styles, std::span<const GpuDrawCommand> commands,
                                   std::span<const std::uint32_t> entries, std::uint32_t base, const BatchedDraws& cpu,
                                   const std::function<bool(std::uint32_t)>& nearEdge);

// cull-compare's reply: both lists, and whether they agree (differences only near edges).
std::string describeCullComparison(const CullListComparison& camera, const CullListComparison& motion);

// ---- The kept table ----

// A frame's tables for the passes: the still draws' kept, the moving draws' made now.
struct CameraCullFrame {
    std::span<const GpuCullRecord> stillRecords;
    std::vector<GpuCullRecord> movingRecords;
    std::span<const GpuCullCandidate> stillCandidates;
    std::vector<GpuCullCandidate> movingCandidates;

    // Still groups, then the moving draws' (their firstCandidate counted after the still candidates).
    std::vector<GpuCullGroup> groups;
    std::vector<CullGroupStyle> styles;

    // The groups in drawing order: by style, then mesh (still before moving), so a pass switches pipeline at most
    // three times.
    std::vector<std::uint32_t> drawOrder;

    // By mesh number: every mesh a draw names, its levels, changesShape set for this frame.
    std::vector<GpuMeshLevels> meshLevels;

    // The glass draws and those with flat faces not glass (the mirror's candidates), for the CPU's own tests.
    std::vector<std::size_t> glass;
    std::vector<std::size_t> flatFaced;

    // Changes whenever the still records or candidates do, so uploads rewrite them only then.
    std::uint64_t stillGeneration = 0;
};

class CameraCullTable {
public:
    /*
    * The frame's tables. The still part is kept when the frame follows the last one (FrameDescription::stillVersion);
    * built ranges repack their records, and the groups are built again only where a draw's mesh, style, glass or flat
    * faces or a mesh's levels changed; otherwise everything is built afresh. changesShape: whether a mesh deformed.
    */
    CameraCullFrame update(const FrameDescription& frame, const std::function<std::uint32_t(MeshHandle)>& indexCount,
                           const std::function<bool(MeshHandle)>& changesShape);

    // How many times the still groups were built (for tests: kept frames build none).
    std::size_t getStillBuilds() const;

private:
    void buildStill(const FrameDescription& frame, const std::function<std::uint32_t(MeshHandle)>& indexCount);
    bool repackRanges(const FrameDescription& frame);

    std::vector<GpuCullRecord> records;
    std::vector<std::uint64_t> keys;
    CullGroups still;
    std::vector<GpuMeshLevels> stillLevels;
    std::vector<std::size_t> stillGlass;
    std::vector<std::size_t> stillFlatFaced;
    std::uint64_t packedVersion = 0;
    std::uint64_t generation = 0;
    std::size_t stillBuilds = 0;
};
