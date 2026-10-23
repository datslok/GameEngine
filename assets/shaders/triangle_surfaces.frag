#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_KHR_shader_subgroup_arithmetic : require

// The main pass writing the reflection surfaces too (render/gpu/screen_reflections.h), for frames with reflections.
#define REFLECTION_SURFACES
#include "scene_surface.glsl"
