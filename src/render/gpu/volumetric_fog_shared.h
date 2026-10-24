#ifndef RENDER_GPU_VOLUMETRIC_FOG_SHARED_H
#define RENDER_GPU_VOLUMETRIC_FOG_SHARED_H

// Volumetric fog's numbers, shared by render/gpu/volumetric_fog.h and the shaders (volumetric_fog.glsl, scene_fog.glsl,
// fog_light.comp, fog_integrate.comp, fog_apply.comp). Only #defines of plain numbers, since GLSL reads them too.

// The slices are spaced evenly in log(depth + this many metres): near the camera about evenly (a slice a few centimetres
// deep), further out exponentially, each a fixed share deeper than the last, as a froxel's width grows with distance.
#define FOG_SLICE_START 1.0

// The settings' limits: the grid's reach (m), its froxels across and down the screen, its slices, and the most of last
// frame's light a froxel keeps.
#define FOG_MIN_RANGE 5.0
#define FOG_MAX_RANGE 2000.0
#define FOG_MIN_GRID_SIDE 8
#define FOG_MAX_GRID_SIDE 512
#define FOG_MIN_SLICES 8
#define FOG_MAX_SLICES 256
#define FOG_MAX_HISTORY_WEIGHT 0.98

// The froxels' sample points move round their middles over this many frames (Halton points), which last frame's light
// carried along averages into a smooth grid.
#define FOG_JITTER_FRAMES 16

// The height fog's closed form along a ray raises e to at most this (the fog growing below its base height, looked down
// into far away), so it stays a float; the light through it is long gone by then.
#define FOG_MAX_EXPONENT 60.0

// The passes' workgroups: the lighting pass 4 x 4 x 4 froxels, the integration 8 x 8 columns, the picture 8 x 8 pixels.
#define FOG_LIGHT_GROUP_SIDE 4
#define FOG_COLUMN_GROUP_SIDE 8
#define FOG_APPLY_GROUP_SIDE 8

#endif
