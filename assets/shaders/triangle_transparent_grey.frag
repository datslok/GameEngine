#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_KHR_shader_subgroup_arithmetic : require

// Glass and blended surfaces (render/gpu/transparency.h) on GPUs without dual-source blending: a grey share passes.
#define TRANSPARENT
#include "scene_surface.glsl"
