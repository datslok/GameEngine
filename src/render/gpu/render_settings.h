#pragma once

#include "core/settings_visitor.h"
#include "render/gpu/bloom.h"
#include "render/gpu/display_output.h"
#include "render/gpu/light_priority.h"
#include "render/gpu/shadow_map.h"
#include "render/gpu/tone_mapping.h"

/*
* Everything about drawing that a game, the debug UI or a player may tune, in one place. The renderer reads it every
* frame, so changes show at once. Fixed capacities (how many lights the shader holds, the atlas size) are not here:
* they size arrays in the shaders and in uniform blocks, so they are compile-time constants (render/gpu/shader_shared.h).
*/
struct RenderSettings {
    LightPrioritySettings lights;
    ShadowSettings shadows;
    BloomSettings bloom;
    ToneMappingSettings toneMapping;
    DisplaySettings display;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(RenderSettings& settings, SettingsVisitor& visitor);
