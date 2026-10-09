#pragma once

#include "math/vec3.h"

/*
* Light components. Colours are 0..1 tints and intensity says how bright, with no upper limit,
* unlike material colours (Pixel), which are how much light a surface reflects.
*/

// Component: a light so far away that its rays are parallel, like the sun. It needs no Transform.
struct DirectionalLight {
    Vec3 direction{0.0f, -1.0f, 0.0f}; // The way the light travels. It does not need to be normalised.
    Vec3 colour{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
};

// Component: a light that shines in every direction from its entity's Transform position and fades out with distance.
struct PointLight {
    Vec3 colour{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f; // The light reaches exactly zero at this distance.

    // The size of the glowing source, softening the light up close like SpotLight::sourceRadius. 1 is a small bulb.
    float sourceRadius = 1.0f;

    // Whether objects block this light. Costs six extra drawings of the scene per frame (one per cube face);
    // only the first shadow-casting point light gets a shadow map.
    bool castsShadows = false;
};

/*
* Component: a point light that shines in a cone, like a flashlight. Position comes from the entity's Transform.
* Full brightness inside innerAngle, fading smoothly to nothing at outerAngle (half-angles from the beam's axis, radians):
* the soft edge real flashlights have because their bulb is not a perfect point.
*/
struct SpotLight {
    Vec3 colour{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    Vec3 direction{0.0f, 0.0f, -1.0f}; // The way the beam points. It does not need to be normalised.
    float innerAngle = 0.26f;          // About 15 degrees.
    float outerAngle = 0.44f;          // About 25 degrees.

    // The size of the glowing source. Falloff is intensity / (distance^2 + sourceRadius^2): a bigger source (a reflector,
    // a lit disc) is gentler up close and still falls off as inverse square far away. 1 matches a point light.
    float sourceRadius = 1.0f;
};

// Component: light that reaches every surface equally, standing in for light bounced around the scene. Several add up.
struct AmbientLight {
    Vec3 colour{0.2f, 0.2f, 0.2f};
};
