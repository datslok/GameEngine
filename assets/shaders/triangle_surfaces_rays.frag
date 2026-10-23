#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_KHR_shader_subgroup_arithmetic : require

// The main pass writing the reflection surfaces too, its shadowed lights tinted by glass (triangle_rays.frag).
#define REFLECTION_SURFACES
#define GLASS_SHADOW_RAYS
#include "scene_surface.glsl"
