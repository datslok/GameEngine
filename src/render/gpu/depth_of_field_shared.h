#ifndef RENDER_GPU_DEPTH_OF_FIELD_SHARED_H
#define RENDER_GPU_DEPTH_OF_FIELD_SHARED_H

// Numbers depth of field (render/gpu/depth_of_field.h) shares with GLSL (depth_of_field.glsl and its passes). Only
// #defines of plain numbers, since GLSL reads them too. Blur radii are in blur pixels: the half-resolution grid's.

// The sensor's height (metres): a 36 x 24 mm "full frame" camera's, from which the field of view sets the focal length.
#define DEPTH_OF_FIELD_SENSOR_HEIGHT 0.024

// The largest blur radius (blur pixels): with its edge within one tile, so the 3 x 3 tiles round a pixel hold everything
// reaching it.
#define DEPTH_OF_FIELD_LARGEST_RADIUS 15.5

// The tiles' side in blur pixels (a radius never reaches past the next tile).
#define DEPTH_OF_FIELD_TILE 16

// The gather's rings round the pixel: ring k holds 8 k samples, so 1 + 4 R (R + 1) samples in all with the pixel's own.
#define DEPTH_OF_FIELD_RINGS 4
#define DEPTH_OF_FIELD_SAMPLES 81

// Half a pixel's width (blur pixels): a disc's edge crossing a pixel covers it from all to none over this either side,
// and the gather reaches this far past the largest radius, so a disc's whole edge is sampled.
#define DEPTH_OF_FIELD_EDGE 0.5

// A blur radius (blur pixels) below which a point counts as in focus: it keeps its sharp pixel and has no near field.
#define DEPTH_OF_FIELD_IN_FOCUS 0.5

// The nearest the lens focuses (metres), and the share of the focal length's inverse it never focuses closer than.
#define DEPTH_OF_FIELD_NEAREST_FOCUS 0.1
#define DEPTH_OF_FIELD_FOCUS_LIMIT 0.5

// The passes' square workgroups; the prepare pass gives each tile DEPTH_OF_FIELD_GROUP x DEPTH_OF_FIELD_GROUP
// invocations, each preparing a square of DEPTH_OF_FIELD_TILE / DEPTH_OF_FIELD_GROUP blur pixels a side.
#define DEPTH_OF_FIELD_GROUP 8

#endif
