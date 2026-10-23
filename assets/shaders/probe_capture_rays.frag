#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_KHR_shader_subgroup_arithmetic : require

// The reflection probe's capture (render/gpu/reflection_probe.h) on GPUs with ray queries: the main pass's shading, with
// the sun's shadow traced by a ray where its cascades (fitted to the camera's view) do not reach, so what is behind the
// camera is not lit through walls and roofs.
#define PROBE_CAPTURE
#define PROBE_CAPTURE_RAYS
#include "scene_surface.glsl"
