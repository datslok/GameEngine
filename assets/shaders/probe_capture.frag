#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_KHR_shader_subgroup_arithmetic : require

// The reflection probe's capture (render/gpu/reflection_probe.h) on GPUs without ray queries: the main pass's shading,
// with every light (the clusters are the camera's) and no reflection surfaces; past the sun's cascades it counts as lit.
#define PROBE_CAPTURE
#include "scene_surface.glsl"
