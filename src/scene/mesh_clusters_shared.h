#ifndef SCENE_MESH_CLUSTERS_SHARED_H
#define SCENE_MESH_CLUSTERS_SHARED_H

// A mesh cluster's most vertices and triangles (scene/mesh_clusters.h), shared with the mesh shaders that draw one a
// workgroup (mesh_cluster_draw.glsl). NVIDIA's advice: 64 vertices, 124 triangles, so the triangles' bytes fit 128-byte
// rows (docs/research/VIRTUALGEOMETRY.md). Only #defines of plain numbers, since GLSL reads them too.
#define MESH_CLUSTER_MAX_VERTICES 64
#define MESH_CLUSTER_MAX_TRIANGLES 124

#endif
