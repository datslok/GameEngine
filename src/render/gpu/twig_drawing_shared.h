#ifndef RENDER_GPU_TWIG_DRAWING_SHARED_H
#define RENDER_GPU_TWIG_DRAWING_SHARED_H

// Twigs drawn as instances of prototypes (render/gpu/twig_records.h, ROADMAP phase 21 stage 3b-iii), shared with the
// shaders (twig_drawing.glsl). Only #defines of plain numbers, since GLSL reads them too.

// A twig entry's number in the cluster passes and mesh shaders: from here (past the placed entries' PLACED_INSTANCE_BASE).
#define TWIG_INSTANCE_BASE 0x60000000u

// A twig group's number in the cluster passes' drawing order: from here, one a region (TWIG_GROUP_BASE + clusters - 1).
#define TWIG_GROUP_BASE 0x40000000u

// Twigs a brick (scene/twig_set.h's twigBrickSize): the twig pass's workgroup.
#define TWIG_BRICK_SIZE 64

// The most clusters a twig's level has; twig entries are kept in a region a cluster count (1 to this), so the cluster
// passes take each region as a group of entries with that many clusters each.
#define TWIG_MOST_LEVEL_CLUSTERS 4

// The most levels a prototype has (a twig of up to 2^7 leaves halved to one).
#define TWIG_MOST_LEVELS 8

// The twig passes' storage buffers (twig_pass.glsl), and the workgroup of the pass gathering their sources.
#define TWIG_PASS_BINDINGS 16
#define TWIG_SOURCES_GROUP_SIZE 64

// The scene set's twig entries and twigs (render/vulkan/scene_layout.h), for the mesh shaders' twig instances.
#define SCENE_TWIG_ENTRIES_BINDING 46
#define SCENE_TWIGS_BINDING 47

#endif
