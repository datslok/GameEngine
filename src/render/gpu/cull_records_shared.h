#ifndef RENDER_GPU_CULL_RECORDS_SHARED_H
#define RENDER_GPU_CULL_RECORDS_SHARED_H

// GPU-driven drawing's numbers (render/gpu/cull_records.h), shared with the shaders (culling.glsl, level_of_detail.glsl).
// Only #defines of plain numbers, since GLSL reads them too.

// A draw's flags (GpuCullRecord::flags): what DrawItem says about how it is drawn and which views take it.
#define CULL_SHADOW_ONLY 1u   // casts a shadow, never drawn by the camera
#define CULL_HIDDEN 2u        // drawn by no view
#define CULL_TRANSPARENT 4u   // glass or blended: left out of every opaque view
#define CULL_CASTS_SHADOWS 8u // taken by shadow views
#define CULL_CUT_OUT 16u      // alpha-tested (batched apart)
#define CULL_DOUBLE_SIDED 32u // drawn without back-face culling (batched apart)
#define CULL_THINNABLE 64u    // its leaf cards can be thinned (DrawItem::thinningEntry)
#define CULL_MOVED 128u       // its matrix changed since last frame (DrawItem::previousModel): the velocity pass redraws it
#define CULL_SWAYS 256u       // it sways in the wind (DrawItem::sway not still), where the sway reaches
#define CULL_FADES 512u       // its levels cross-fade in the camera's view (DrawItem::fadesLevels)
#define CULL_CLUSTER_LEVELS 1024u // its mesh has cluster levels (DrawItem::clusterLevels): kept whole where a view draws them

// The meshes a draw can be drawn with: its own and up to three levels (scene/mesh_level.h, meshLevelSlots).
#define CULL_LEVEL_SLOTS 4

// A level slot without a mesh (a tree past its last level draws nothing), and a slot chosen for nothing.
#define CULL_NO_MESH 4294967295u
#define CULL_NO_SLOT -1

// A chunk's flags (GpuCullChunk::flags, GPU-driven drawing's stage 7b): what its members may be drawn by.
#define CULL_CHUNK_SEEN 1u   // a member a camera view may take (not shadow-only, hidden or transparent)
#define CULL_CHUNK_CASTS 2u  // a member a shadow view may take (casts shadows, not hidden or transparent)
#define CULL_CHUNK_ALWAYS 4u // a member of unknown size: the chunk is never culled as one

// A segment of moving draws: in no chunk, its draws always tested one by one.
#define CULL_NO_CHUNK 4294967295u

// A chunk's box is its members' spheres' box grown by this many metres and this share of its farthest coordinate, so
// rounding can never cull a chunk holding a draw the draw's own test would keep.
#define CULL_CHUNK_MARGIN 0.01
#define CULL_CHUNK_RELATIVE_MARGIN 0.00001

// GpuLevelView::flags.
#define CULL_LEVELS_ENABLED 1u
#define CULL_LEVELS_IMPOSTORS 2u
#define CULL_LEVELS_ORTHOGRAPHIC 4u
#define CULL_LEVELS_CLUSTERS 8u // draws mesh clusters by their levels (LevelView::clusterLevels)

#endif
