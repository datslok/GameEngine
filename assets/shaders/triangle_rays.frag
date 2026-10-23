#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_KHR_shader_subgroup_arithmetic : require

// The main pass on GPUs with ray queries while glass is in the world: its shadowed lights tinted by the glass between
// each point and the light, found by rays (render/gpu/transparency.h).
#define GLASS_SHADOW_RAYS
#include "scene_surface.glsl"
