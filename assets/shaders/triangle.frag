#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_KHR_shader_subgroup_arithmetic : require

// The shading itself (scene_surface.glsl), shared with the reflection probe's ray-traced capture.
#include "scene_surface.glsl"
