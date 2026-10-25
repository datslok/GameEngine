#ifndef SCENE_DEFORMING_MESH_SHARED_H
#define SCENE_DEFORMING_MESH_SHARED_H

// Shaping a deforming mesh (scene/deforming_mesh.cpp and its GPU twin, deform.glsl). Only #defines of plain numbers,
// since GLSL includes this file too.

// Below this a summed normal or tangent, squared, is taken as none: a shape squashed flat or to a point.
#define DEFORM_SMALLEST_DIRECTION 1e-12

// A normal whose x is below this takes its fallback tangent across x, otherwise across y.
#define DEFORM_PERPENDICULAR_SWITCH 0.9

#endif
