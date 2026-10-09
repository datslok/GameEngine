#pragma once

#include "ecs/entity.h"
#include "math/frustum.h"
#include "math/vec3.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/shadow_map.h"
#include "scene/lighting.h"

#include <vector>

/*
* Light priority: the shader has seats for a few lights of each kind and the shadow atlas for fewer still, so when a
* scene has more, the ones that matter most for this frame's picture get them.
* - A light whose range sphere is entirely outside the view cannot light a visible pixel, so it gets nothing.
* - The rest are ranked by priority (a game's own marking, higher always wins), then by how brightly they light the
*   focus point (where the action is), and the top ones are seated.
* - Of the seated point lights and spotlights that cast shadows, the top ones of each kind get shadows.
* - Hysteresis: a light that had a seat (or a shadow) last frame counts incumbentAdvantage times as important, so a
*   rival must be clearly brighter to take it, and two similar lights do not swap every frame as the focus moves.
*   It only applies between lights of equal priority.
* - Fading: when a seat or a shadow changes hands, the light losing it fades out over lightFadeSeconds while it still
*   holds it, and only then does the winner fade in, so the limits always hold and nothing pops. A light that was not
*   on screen last frame (just switched on, or just come into view) starts at full strength instead: nothing showed
*   it before, so there is nothing to blend from. A light that leaves the view is dropped at once, for the same reason.
*/

// How much more important a light counts for keeping what it had last frame. In distance terms a rival must be about
// 11% closer (sqrt(1.25) = 1.118) to take a seat or a shadow from an equally bright light.
inline constexpr float incumbentAdvantage = 1.25f;

// How long a light or its shadow takes to fade fully in or out during a handover.
inline constexpr float lightFadeSeconds = 0.25f;

// A light holding a seat, and how far it is into the seat and into a shadow (0..1; shadow 0 = none).
struct LightFade {
    Entity entity;
    float light = 0.0f;
    float shadow = 0.0f;
};

// What last frame decided, recognising lights by their entity: who was on screen (could be drawn and reached into
// view), and who held seats and shadows, and how far faded.
struct LightHistory {
    std::vector<Entity> inView;
    std::vector<LightFade> points;
    std::vector<LightFade> spots;
};

// The lights to draw this frame (within the shader's limits; castsShadows is on only for lights that hold a shadow,
// fade and shadowFade say how far in they are), and the history to pass to the next frame.
struct PrioritizedLighting {
    FrameLighting lighting;
    LightHistory history;

    // How many point lights could be drawn and reach into view, before the seats were handed out (for the debug readout).
    std::size_t pointLightsInView = 0;
};

/*
* How brightly a light lights the focus point: the shader's falloff, intensity / (distance^2 + sourceRadius^2), times
* the brightest channel of its colour. The shader's range window is left out on purpose: it is zero beyond the range,
* so every light out of reach of the focus would tie at zero, while this way they still rank by distance.
*/
float lightImportance(const Vec3& lightPosition, const Vec3& colour, float intensity, float sourceRadius, const Vec3& focus);

// How many lights of each kind get a seat in the shader, and how many of those a shadow. The defaults are the engine's
// limits; shadows may be capped below the seats (then lights can lose their shadow but keep shining), never above.
struct LightLimits {
    int pointSeats = maxPointLights;
    int spotSeats = maxSpotLights;
    int shadowedPoints = maxShadowedPointLights;
    int shadowedSpots = maxShadowedSpotLights;
};

// Choose this frame's lights. view is the camera's frustum; previous is the history this function returned last frame;
// elapsedSeconds is the time since then, which moves the fades along.
PrioritizedLighting prioritizeLights(const FrameLighting& lighting, const Vec3& focus, const Frustum& view, const LightHistory& previous,
                                     float elapsedSeconds, const LightLimits& limits = LightLimits{});
