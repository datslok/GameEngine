#pragma once

#include "render/gpu/camera_culling_shared.h"
#include "render/gpu/cull_records.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/mesh_pool.h"
#include "render/gpu/planar_reflection.h"
#include "scene/frame_description.h"
#include "scene/world_cells.h"

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

// A group: its run of segments (each a chunk's candidates of the group, in chunk order) and its mesh's indices where
// they are (mesh_pool.h), for its indirect command. Must match camera_culling.glsl's CullGroup.
struct GpuCullGroup {
    std::uint32_t firstSegment = 0;
    std::uint32_t segments = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t firstIndex = 0;
    std::int32_t vertexOffset = 0;
    std::uint32_t unused[3] = {0, 0, 0};
};

static_assert(sizeof(GpuCullGroup) == 32, "GpuCullGroup must match camera_culling.glsl's CullGroup");

/*
* A group's candidates in one chunk (GPU-driven drawing's stage 7b): a run of the candidate list and the chunk (cullNoChunk
* for moving draws: always tested one by one). Segments run group by group, each group's in chunk order. Must match
* camera_culling.glsl's CullSegment.
*/
struct GpuCullSegment {
    std::uint32_t firstCandidate = 0;
    std::uint32_t candidates = 0;
    std::uint32_t chunk = 0;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuCullSegment) == 16, "GpuCullSegment must match camera_culling.glsl's CullSegment");

// What the segment passes found for a view, read by the later passes: its visible candidates and segments, and the
// workgroups of visible candidates. Must match camera_culling.glsl's CullViewWork.
struct GpuCullViewWork {
    std::uint32_t visibleCandidates = 0;
    std::uint32_t visibleSegments = 0;
    std::uint32_t candidateBlocks = 0;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuCullViewWork) == 16, "GpuCullViewWork must match camera_culling.glsl's CullViewWork");

// How a group is drawn: its mesh, its pipeline (cut-out, double-sided), and whether its mesh is in the pools.
struct CullGroupStyle {
    MeshHandle mesh;
    bool cutOut = false;
    bool doubleSided = false;
    bool pooled = false;
};

// The ways of drawing (batchByMesh's styles: plain, double-sided, cut-out, double-sided cut-out), one run each.
inline constexpr std::size_t cullStyles = CAMERA_CULL_STYLES;
static_assert(CAMERA_CULL_DRAW_COUNTS == 2 * CAMERA_CULL_STYLES, "A packed count for each style of both lists");

inline constexpr std::size_t cullScratchViews = CAMERA_CULL_SCRATCH_VIEWS;

// A view culled (the camera, a probe face, the mirror or a shadow tile), read by every pass. Must match
// camera_culling.glsl's CameraCull.
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
    std::uint32_t view = 0;                   // its number in the dispatch: its counts' and packed counts' place
    std::uint32_t runFirst[cullStyles] = {};  // each style's pooled groups' first place in the drawing order
    std::uint32_t runCount[cullStyles] = {};  // and how many
    std::uint32_t slices[4] = {};             // CAMERA_CULL_SLICE_*: its parts of the shared working buffers
    float reach[4] = {0.0f, 0.0f, 0.0f, -1.0f}; // a shadow tile's light reach (xyz centre, w radius; below 0 everywhere)
    float smallestCaster = 0.0f;              // a shadow tile's: casters of a smaller radius left out
    std::int32_t cascade = -1;                // a shadow tile's sun cascade (ShadowTile::sunCascade; -1 none)
    std::uint32_t segments = 0;               // the segments every view's passes walk
    std::uint32_t segmentBlocks = 0;          // workgroups of segments
    std::uint32_t segmentSlices[2] = {};      // CAMERA_CULL_SEGMENT_SLICE_*: its parts of the segment passes' buffers
    float thinning[2] = {0.0f, 0.0f};         // a sun cascade's thinning band (ShadowTile::thinStart, thinEnd)
};

static_assert(sizeof(GpuCameraCull) == 256, "GpuCameraCull must match camera_culling.glsl's CameraCull");

// What the passes count (CAMERA_CULL_COUNTS words): draws by level slot, the camera's and the velocity pass's totals.
struct GpuCameraCullCounts {
    std::uint32_t values[CAMERA_CULL_COUNTS] = {};
};

static_assert(sizeof(GpuCameraCullCounts) == 32, "GpuCameraCullCounts must match camera_cull_flags.comp's Counts");

/*
* Groups of draws first to first + count of draws, numbered as draw indices into the whole list: every draw not
* transparent, each mesh slot it has (glass is sorted on the CPU; shadow-only and hidden draws are candidates, so a draw
* hidden or shown keeps its groups, and the GPU drops them by their flags). placement says where a mesh's indices are.
*/
struct CullGroups {
    std::vector<GpuCullGroup> groups;
    std::vector<CullGroupStyle> styles;
    std::vector<GpuCullCandidate> candidates;
    std::vector<GpuCullSegment> segments; // one a group, in no chunk
};

CullGroups buildCullGroups(std::span<const DrawItem> draws, std::size_t first, std::size_t count,
                           const std::function<MeshPlacement(MeshHandle)>& placement);

/*
* The groups in drawing order: the pooled ones first, by style then mesh (stable, so still groups before moving ones),
* each style a run drawn by one multi-draw; then those with their own buffers (deforming meshes), drawn one by one.
*/
struct CullDrawOrder {
    std::vector<std::uint32_t> order;
    std::array<std::uint32_t, cullStyles> runFirst{};
    std::array<std::uint32_t, cullStyles> runCount{};
    std::uint32_t pooled = 0;
};

CullDrawOrder cullDrawOrder(std::span<const CullGroupStyle> styles);

/*
* The view's record. motion: the velocity pass's list is built; sway: things sway this frame (the wind's settings on),
* fading out at swayFadeEnd.
*/
GpuCameraCull packCameraCull(const Frustum& frustum, const LevelView& levels, bool motion, bool sway, float swayFadeEnd);

/*
* A capture's view: what it can see and its levels. A probe face from its cycle's origin (faceSize pixels across a
* quarter turn, so coarser levels than the camera's); the mirror through its camera-relative view-projection
* (planarViewProjection), with its plane for its near side.
*/
struct CaptureCullView {
    Frustum frustum;
    LevelView levels;
};

CaptureCullView probeFaceCullView(int face, const Vec3& origin, float farPlane, float faceSize, const LevelOfDetailSettings& settings);
CaptureCullView mirrorCullView(const Mat4& mirrorViewProjection, const Vec3& cameraOrigin, const MirrorPlane& mirror, float pictureHeight,
                               float verticalFov, const LevelOfDetailSettings& settings);

// A capture's record: no velocity list, nothing tested for sway.
GpuCameraCull packCaptureCull(const CaptureCullView& capture);

/*
* The view's place in one dispatch of views: its number, the table's counts and drawing order's runs, and its slices
* of the shared working buffers, each view's after the one before's (the same sizes for every view); the scratch ones
* (flags, offsets, block sums, the segments' offsets, block sums and visible list) by its place in its batch of
* cullScratchViews. candidates: the candidate list's length (an upper bound on any view's visible candidates).
*/
void placeCullView(GpuCameraCull& view, std::uint32_t index, std::uint32_t candidates, std::uint32_t segments, const CullDrawOrder& order);

// What views views' slices take together (scratch for at most one batch), in elements of each buffer.
struct CullViewSizes {
    std::size_t flags = 0;         // flags and offsets, one a candidate
    std::size_t blocks = 0;        // block sums, the totals after each view's
    std::size_t commands = 0;      // commands and packed commands, one a group
    std::size_t places = 0;        // packed places, the total after each view's
    std::size_t segments = 0;      // the segments' offsets and the visible list, one a segment
    std::size_t segmentBlocks = 0; // the segments' block sums, the totals after each view's
};

CullViewSizes cullViewSizes(std::size_t views, std::uint32_t candidates, std::uint32_t segments, const CullDrawOrder& order);

// ---- Mirrored by camera_culling.glsl ----

// Whether part of the sphere is nearer the eye than the fade's end (drawSways).
bool swayReaches(const float bounds[4], const Vec3& eye, float fadeEnd);

/*
* Whether a shadow tile takes a draw: a solid caster, not hidden (shadow-only kept), touching the light's reach, maybe
* in the tile's view, at least the smallest caster's size (unknown sizes kept) and within its cascades (castersInReach,
* shadowCasters, castersAtLeast, castersForCascade), and not thinned out (shadowThinKeeps).
*/
bool shadowTakes(const GpuCullRecord& record, const GpuCameraCull& view);

/*
* The share of casters like this one a sun cascade keeps: 1 unless the cascade is the last the caster is limited to
* (GpuCullRecord::shadowCascades) and has a thinning band (thinning x above y); then 1 from x in from the box's sides
* (measured in radii, as cascadeEdgeDistance's sides), falling evenly to 0 at y.
*/
float shadowThinShare(const GpuCullRecord& record, const GpuCameraCull& view);

// The caster's own fixed number in [0, 1): PCG3D of its sphere's centre's bits, so it never changes while it stands.
float shadowThinNumber(const GpuCullRecord& record);

// Whether the cascade keeps the caster: its number below the share (a share of 1 keeps every caster, 0 none).
bool shadowThinKeeps(const GpuCullRecord& record, const GpuCameraCull& view);

// Whether a box (a chunk's) may be inside all six planes: its corner farthest along each plane's normal tested.
bool boxInFrustum(const GpuFrustum& frustum, const float minimum[3], const float maximum[3]);

/*
* Whether a view may take any of a chunk's draws: a camera view one it can see (CULL_CHUNK_SEEN), a shadow tile a caster
* (CULL_CHUNK_CASTS) whose box touches the light's reach; then the box in the frustum. A chunk with a draw of unknown
* size always may. Never false where cameraCandidateFlags or shadowTakes would take a member (the box holds their
* spheres and its margin outgrows rounding).
*/
bool chunkInView(const GpuCullChunk& chunk, const GpuCameraCull& view);

// The candidate's CAMERA_CULL_* flags: drawn, moving and thinned, as visibleDraws, chosenMesh and drawSways choose, and
// fading where its draw crosses a switch in the fade band (levelFadeSlots; both its slots drawn); for a shadow tile,
// drawn as shadowTakes says (moved casters moving instead where it keeps them apart), never thinned, never fading.
std::uint32_t cameraCandidateFlags(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuCameraCull& view, std::uint32_t slot);

// The candidate's fade field (render/gpu/level_fade.h): 0 where it does not fade.
std::uint32_t cameraCandidateFade(const GpuCullRecord& record, const GpuMeshLevels& levels, const GpuCameraCull& view, std::uint32_t slot);

// The draw list entry a flagged candidate writes: its draw, the thinned bit (by its flags), its fade field (the write
// pass reads a record only for a candidate flagged fading).
std::uint32_t cameraCandidateEntry(std::uint32_t draw, std::uint32_t flags, std::uint32_t fade);

// ---- The passes' steps on the CPU ----

/*
* The camera's and the velocity pass's entries (from their bases), every group's commands, as the GPU writes them, and
* each list's commands packed (camera_cull_compact.comp): within each style's run of the drawing order (view.runFirst,
* runCount) the commands drawing something, in order, from the run's first place, and how many (style by style).
*/
struct CameraCullResult {
    std::vector<std::uint32_t> mainEntries;
    std::vector<std::uint32_t> motionEntries;
    std::vector<GpuDrawCommand> mainCommands;
    std::vector<GpuDrawCommand> motionCommands;
    std::array<std::uint32_t, CULL_LEVEL_SLOTS> levels{};
    std::vector<GpuDrawCommand> packedMain;
    std::vector<GpuDrawCommand> packedMotion;
    std::array<std::uint32_t, cullStyles> mainCounts{};
    std::array<std::uint32_t, cullStyles> motionCounts{};
};

// What the passes read: records by draw, level tables by mesh, chunks by number, groups, segments and candidates.
struct CullTables {
    std::span<const GpuCullRecord> records;
    std::span<const GpuMeshLevels> levels;
    std::span<const GpuCullChunk> chunks;
    std::span<const GpuCullGroup> groups;
    std::span<const GpuCullSegment> segments;
    std::span<const GpuCullCandidate> candidates;
};

/*
* The segments whose chunk is in view (chunkInView; no chunk always), their candidates one after another in segment
* order (the visible candidates), then the flags, their prefix sum, the writes (each group's command from the sums at
* its first segment and past its last) and the packing.
*/
CameraCullResult cullCameraOnCpu(const CullTables& tables, std::span<const std::uint32_t> drawOrder, const GpuCameraCull& view);

// The packing alone: each style's run of commands (in drawing order) to those drawing something, and their counts.
void packCullCommands(std::span<const GpuDrawCommand> commands, std::span<const std::uint32_t> drawOrder, const GpuCameraCull& view,
                      std::vector<GpuDrawCommand>& packed, std::array<std::uint32_t, cullStyles>& counts);

/*
* Whether the draw's choice is within rounding of changing (a level's error within 1e-4 of the budget, its sphere
* within a millimetre of a plane or of the sway's reach, its thinning number within 1e-4 of its share), where the GPU and
* the CPU may each be right.
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

// A list compared, with its name in the reply ("Camera", "Probe face 2", "Mirror", "Shadow tile 3").
struct NamedCullComparison {
    std::string name;
    CullListComparison list;
};

// cull-compare's reply: every list compared, and whether they agree (differences only near edges).
std::string describeCullComparison(std::span<const NamedCullComparison> lists);

// ---- The kept table ----

// A frame's tables for the passes: the still draws' kept, the moving draws' made now.
struct CameraCullFrame {
    std::span<const GpuCullRecord> stillRecords;
    std::vector<GpuCullRecord> movingRecords;

    // The still draws' chunks (slot / chunkSlots), kept.
    std::span<const GpuCullChunk> stillChunks;
    std::size_t chunkSlots = 1;

    // The still candidates kept a chunk at a time (runs with room between them, unused there), then the moving draws'
    // after all of them.
    std::span<const GpuCullCandidate> stillCandidates;
    std::vector<GpuCullCandidate> movingCandidates;

    // The still segments (group by group, chunk order within), then the moving draws' (one a group, no chunk).
    std::span<const GpuCullSegment> stillSegments;
    std::vector<GpuCullSegment> movingSegments;

    // Still groups, then the moving draws' (their segments counted after the still ones).
    std::vector<GpuCullGroup> groups;
    std::vector<CullGroupStyle> styles;

    // The groups in drawing order (cullDrawOrder).
    CullDrawOrder drawOrder;

    // By mesh number: every mesh a draw names, its levels, changesShape set for this frame.
    std::vector<GpuMeshLevels> meshLevels;

    // The glass draws and those with flat faces not glass (the mirror's candidates), for the CPU's own tests.
    std::vector<std::size_t> glass;
    std::vector<std::size_t> flatFaced;

    // Changes whenever the still records, chunks, candidates or segments do, by one a frame that changed them.
    std::uint64_t stillGeneration = 0;

    // What changed in the still part since the frame before, for the shadow cache (render/gpu/shadow_culling.h): built
    // afresh (anything may have), else each draw built in place, its record as it was and as it is.
    bool stillRebuilt = false;
    std::vector<GpuCullRecord> stillChanges;

    // What was packed this frame, for the lasting buffers (render/gpu/lasting_buffers.h), joined: the still records, the
    // chunks, the candidates' runs (all of each when built afresh, none when kept), and whether the segments changed.
    std::vector<DrawRange> stillRepacked;
    std::vector<DrawRange> chunksRepacked;
    std::vector<DrawRange> candidatesRepacked;
    bool segmentsChanged = false;

    // Everything the passes read, still and moving joined (the moving parts copied after the still ones).
    struct Joined {
        std::vector<GpuCullRecord> records;
        std::vector<GpuCullCandidate> candidates;
        std::vector<GpuCullSegment> segments;
    };

    Joined join() const;
    CullTables tables(const Joined& joined) const;
};

/*
* The still draws' culling tables kept from frame to frame, and the moving draws' made each frame. The still candidates
* live a chunk at a time (each chunk's in a run with spare room, SlotBlocks), sorted by group then draw within it, so a
* chunk whose draws changed (a cell rebuilt, scene/still_draws.h) rebuilds its own candidates; the segment table and
* the still groups are then made again from every chunk's runs (growing with the segments, a few for every 64 draws).
*/
class CameraCullTable {
public:
    /*
    * The frame's tables. The still part is kept when the frame follows the last one (FrameDescription::stillVersion);
    * built ranges (covering any slots added at the end) repack their records and chunks, and the candidates of chunks
    * where a draw's mesh, style, glass or flat faces changed; a mesh's levels changed builds everything afresh, as does a
    * frame not following. changesShape: whether a mesh deformed. Throws std::length_error past drawListInstances draws.
    */
    CameraCullFrame update(const FrameDescription& frame, const std::function<MeshPlacement(MeshHandle)>& placement,
                           const std::function<bool(MeshHandle)>& changesShape);

    // How many times the still part was built afresh, and how many chunks' candidates were built (for tests).
    std::size_t getStillBuilds() const;
    std::size_t getChunkBuilds() const;

private:
    // A chunk's candidates: their run in the list (first, room) and how many; its segments, one a group key.
    struct ChunkCandidates {
        std::size_t first = 0;
        std::size_t room = 0;
        std::size_t count = 0;
        std::vector<std::pair<std::uint64_t, GpuCullSegment>> segments;
    };

    void buildStill(const FrameDescription& frame, const std::function<MeshPlacement(MeshHandle)>& placement, CameraCullFrame& result);
    bool repackRanges(const FrameDescription& frame, const std::function<MeshPlacement(MeshHandle)>& placement, CameraCullFrame& result);
    void buildChunkCandidates(const FrameDescription& frame, std::size_t chunk);
    void buildSegments(const std::function<MeshPlacement(MeshHandle)>& placement);

    std::vector<GpuCullRecord> records;
    std::vector<std::uint64_t> keys;
    std::size_t chunkSlots = 1;
    std::vector<GpuCullChunk> chunks;
    std::vector<ChunkCandidates> chunkCandidates;
    std::vector<GpuCullCandidate> candidates;
    SlotBlocks candidateRuns;
    std::vector<GpuCullSegment> segments;
    std::vector<GpuCullGroup> groups;
    std::vector<CullGroupStyle> styles;
    std::vector<GpuMeshLevels> stillLevels;
    std::vector<std::size_t> stillGlass;
    std::vector<std::size_t> stillFlatFaced;
    std::uint64_t packedVersion = 0;
    std::uint64_t generation = 0;
    std::size_t stillBuilds = 0;
    std::size_t chunkBuilds = 0;
};
