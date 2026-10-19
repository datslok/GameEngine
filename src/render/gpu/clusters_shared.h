#ifndef RENDER_GPU_CLUSTERS_SHARED_H
#define RENDER_GPU_CLUSTERS_SHARED_H

// Clustered shading's grid, shared with the shaders (clusters.glsl, cluster_lights.comp, triangle.frag). Only #defines
// of plain numbers, since GLSL includes this file too; C++ reads them as typed constants in render/gpu/clusters.h.

// Each screen tile is this many pixels square (the last column and row are cut off by the screen's edge).
#define CLUSTER_TILE_PIXELS 64

// Depth slices from the near plane to the far plane, spaced exponentially.
#define CLUSTER_DEPTH_SLICES 24

// One bit per light: 32 words for the 1024 point lights, 8 for the 256 spotlights (maxPointLights and maxSpotLights,
// light_uniforms.h, checked there), point lights first. 40 words, 160 bytes per cluster.
#define CLUSTER_POINT_WORDS 32
#define CLUSTER_SPOT_WORDS 8
#define CLUSTER_WORDS 40

// How much each cluster's box is grown on every side, as a share of its far depth, so a pixel whose depth or tile rounds
// into a neighbouring cluster still finds every light that reaches it.
#define CLUSTER_BOUNDS_MARGIN 0.001

// Invocations per workgroup of cluster_lights.comp: 32 clusters, each testing one word's 32 lights at a time.
#define CLUSTER_GROUP_SIZE 32

#endif
