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

// The meshes a draw can be drawn with: its own and up to three levels (scene/mesh_level.h, meshLevelSlots).
#define CULL_LEVEL_SLOTS 4

// A level slot without a mesh (a tree past its last level draws nothing), and a slot chosen for nothing.
#define CULL_NO_MESH 4294967295u
#define CULL_NO_SLOT -1

// GpuLevelView::flags.
#define CULL_LEVELS_ENABLED 1u
#define CULL_LEVELS_IMPOSTORS 2u
#define CULL_LEVELS_ORTHOGRAPHIC 4u

#endif
