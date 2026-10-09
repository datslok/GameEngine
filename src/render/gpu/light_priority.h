#pragma once

#include "ecs/entity.h"
#include "math/frustum.h"
#include "math/vec3.h"
#include "scene/lighting.h"

#include <vector>

/*
* Light priority: the shader has seats for a few lights of each kind and the shadow atlas for fewer still, so when a
* scene has more, the ones that matter most for this frame's picture get them.
* - A light whose range sphere is entirely outside the view cannot light a visible pixel, so it gets nothing.
* - The rest are ranked by how brightly they light the focus point (where the action is), and the top ones are seated.
* - Of the seated point lights and spotlights that cast shadows, the top ones of each kind get shadows.
* - Hysteresis: a light that had a seat (or a shadow) last frame counts incumbentAdvantage times as important, so a
*   rival must be clearly brighter to take it, and two similar lights do not swap every frame as the focus moves.
*/

// How much more important a light counts for keeping what it had last frame. In distance terms a rival must be about
// 11% closer (sqrt(1.25) = 1.118) to take a seat or a shadow from an equally bright light.
inline constexpr float incumbentAdvantage = 1.25f;

// Who had which seat and shadow last frame. Lights are recognised by their entity.
struct LightHistory {
    std::vector<Entity> litPoints;
    std::vector<Entity> shadowedPoints;
    std::vector<Entity> litSpots;
    std::vector<Entity> shadowedSpots;
};

// The lights to draw this frame (within the shader's limits; castsShadows is on only for lights that get a shadow), and
// the history to pass to the next frame.
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

// Choose this frame's lights. view is the camera's frustum; previous is the history this function returned last frame.
PrioritizedLighting prioritizeLights(const FrameLighting& lighting, const Vec3& focus, const Frustum& view, const LightHistory& previous);
