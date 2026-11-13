#pragma once

#include "core/settings_visitor.h"
#include "render/gpu/ambient_occlusion.h"
#include "render/gpu/bloom.h"
#include "render/gpu/camera_finish.h"
#include "render/gpu/contact_shadows.h"
#include "render/gpu/ddgi.h"
#include "render/gpu/display_output.h"
#include "render/gpu/light_priority.h"
#include "render/gpu/mirrored_light.h"
#include "render/gpu/normal_mapping_shared.h"
#include "render/gpu/ray_tracing.h"
#include "render/gpu/screen_reflections.h"
#include "render/gpu/shadow_map.h"
#include "render/gpu/sky_visibility.h"
#include "render/gpu/taa.h"
#include "render/gpu/tone_mapping.h"
#include "render/gpu/transparency.h"
#include "render/gpu/upscaling.h"
#include "render/gpu/volumetric_fog.h"

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

    // The camera's finish (render/gpu/camera_finish.h): white balance, the grade and the lens, all neutral or off by
    // default.
    CameraSettings camera;
    DisplaySettings display;
    GlobalIlluminationSettings globalIllumination;
    TaaSettings taa;
    UpscalingSettings upscaling;

    // Screen-space reflections (render/gpu/screen_reflections.h): they need the anti-aliasing on (TAA, FSR or DLSS),
    // whose finished picture of last frame they reflect.
    ReflectionSettings reflections;

    // Lights reflected off mirrors onto what they point at (render/gpu/mirrored_light.h): on GPUs with ray queries.
    MirroredLightSettings mirroredLights;

    // Ambient occlusion (render/gpu/ambient_occlusion.h): the light from all around dimmed in creases and corners, from
    // the depth prepass's depths.
    AmbientOcclusionSettings ambientOcclusion;

    // Contact shadows (render/gpu/contact_shadows.h): the sun's short shadows the shadow map misses, marched through the
    // depth prepass's depths.
    ContactShadowSettings contactShadows;

    // Sky visibility (render/gpu/sky_visibility.h): the flat ambient light dimmed by how much sky each point sees, from a
    // grid round the camera baked on the CPU against the static level, so rooms go dark without the light probes.
    SkyVisibilitySettings skyVisibility;

    // Glass and blended surfaces (render/gpu/transparency.h): the reactive mask that keeps anti-aliasing from smearing them.
    TransparencySettings transparency;

    // Volumetric fog (render/gpu/volumetric_fog.h): the air lit by the lights, when the frame has a Fog.
    FogSettings fog;

    // Depth first, then shading only the nearest surface (render/gpu/depth_prepass.h). Off shades whatever passes the
    // depth test as it is drawn, for comparing the two in the GPU timings.
    bool depthPrepass = true;

    // Each pixel loops only over the lights listed for its cluster (render/gpu/clusters.h) instead of every light in the
    // frame; both add the same lights in the same order, so the picture is identical. Off is for comparing the timings.
    bool clusteredShading = true;

    // A debug view: each pixel tinted by how many lights its cluster lists, black for none, then blue, green, yellow and
    // red at render/gpu/clusters.h's full scale and above.
    bool clusterHeatmap = false;

    // Normal map filtering (render/gpu/normal_mapping.h, toksvigRoughness): bumps a normal map's smaller mips average away
    // become extra roughness, so distant bumpy surfaces do not sparkle. Off shows the raw mips, for comparing.
    bool normalMapFiltering = true;

    // Geometric specular anti-aliasing (render/gpu/normal_mapping.h, geometricRoughness): a curved surface's roughness
    // widened by how fast its normal turns from pixel to pixel, so highlights narrower than a pixel (the sun on a mirror
    // ball) neither sparkle nor swing in brightness as the camera moves. Off shows the raw highlights, for comparing.
    bool specularAntiAliasing = true;

    // Its pixel footprint's variance: Tokuyoshi and Kaplanyan's 0.25 by default; more steadies sub-pixel highlights further
    // (a mirror ball's sun swings 1.17 times at 0.25, 1.04 at 0.5, against 3.6 without) and softens curved highlights.
    float specularAntiAliasingVariance = static_cast<float>(NORMAL_GEOMETRIC_VARIANCE);

    // Hardware ray tracing, every feature that traces rays at once: the light probes (all bounce light, since there is
    // no software tracer yet), ray-traced reflections, mirrored lights, glass shadows, the reflection probe's traced sun
    // and the ray debug view. Off by default, so the engine draws as on a GPU without ray queries, where it is ignored;
    // each feature keeps its own switch, read only while this is on.
    bool rayTracing = false;

    // A debug view in place of the lit scene: each pixel's first hit by a hardware-traced ray (render/gpu/ray_tracing.h)
    // as its normal, albedo or front or back face, to check the ray tracing scene against the drawn one. Only while ray
    // tracing is on; ignored otherwise.
    RayDebugView rayDebugView = RayDebugView::Off;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(RenderSettings& settings, SettingsVisitor& visitor);

// Whether a frame may trace rays: ray tracing on and a GPU with ray queries (rayQueries).
bool tracesRays(const RenderSettings& settings, bool rayQueries);
