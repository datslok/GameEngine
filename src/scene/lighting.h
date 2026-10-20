#pragma once

#include "ecs/world.h"
#include "math/vec3.h"
#include "scene/atmosphere.h"
#include "scene/light.h"
#include "scene/probe_volume.h"

#include <optional>
#include <vector>

// A point light together with where it is this frame, and whose it is, so the renderer can recognise it next frame
// (Entity{} for a light that belongs to no entity).
struct PlacedPointLight {
    Vec3 position;
    PointLight light;
    Entity entity{};

    // How far into its seat (fade) and shadow (shadowFade) the light is, 0..1, while the renderer hands seats and shadows
    // over between lights. Collected lights start at 1; light priority lowers them during a handover.
    float fade = 1.0f;
    float shadowFade = 1.0f;
};

// A spotlight together with where it is this frame, and whose it is.
struct PlacedSpotLight {
    Vec3 position;
    SpotLight light;
    Entity entity{};

    // Like PlacedPointLight's: how far into its seat and its shadow the light is during a handover.
    float fade = 1.0f;
    float shadowFade = 1.0f;
};

// Every light the renderer needs for one frame, with no GPU types.
struct FrameLighting {
    Vec3 ambient{0.0f, 0.0f, 0.0f};
    Vec3 background{0.0f, 0.0f, 0.0f}; // the SkyBackground, where nothing is drawn
    std::vector<DirectionalLight> directionalLights;
    std::vector<PlacedPointLight> pointLights;
    std::vector<PlacedSpotLight> spotLights;
    std::optional<ProbeCascades> probeCascades; // global illumination's probes and what they follow (the first, if several)
    std::vector<ProbeEvent> probeEvents;   // big changes signalled since the last frame, in order
    std::optional<Atmosphere> atmosphere;  // the sky and the air the light crosses (the first, if several)
};

// Gather the World's lights for drawing at alpha (0..1) between the last two ticks.
FrameLighting collectLighting(World& world, float alpha);

// Tell the light probes about a big change: it reaches the next frame's lighting, then clearProbeEvents removes it.
void signalProbeEvent(World& world, const ProbeEvent& event);

// Remove every signalled event, once the frame that carries them is described (the engine does this every frame).
void clearProbeEvents(World& world);
