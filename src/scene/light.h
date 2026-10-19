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

    // Whether objects block this light. Its shadow covers the renderer's ShadowSettings::directionalDistance in front of the
    // camera, in cascades; only the brightest directional light gets one.
    bool castsShadows = true;

    // Half the angle its disc spans as seen from the ground, in radians, which decides how soft its shadows are: behind a
    // blocker the soft edge widens by tan(angularRadius) per metre. The sun's (and, nearly, the full moon's) by default:
    // 0.267 degrees, a disc about half a degree across. 0 gives the sharpest shadows.
    float angularRadius = 0.00465f;
};

// Component: a light that shines in every direction from its entity's Transform position and fades out with distance.
struct PointLight {
    Vec3 colour{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f; // The light reaches exactly zero at this distance.

    // The size of the glowing source, softening the light up close like SpotLight::sourceRadius. 1 is a small bulb.
    float sourceRadius = 1.0f;

    // How big the part that really glows is (the bulb), which decides how soft its shadows are: the further a surface
    // is behind the thing shadowing it, the wider the blurred edge. Separate from sourceRadius, which only shapes the
    // falloff up close and is often tuned much larger than any real bulb. 0 gives the sharpest shadows.
    float emitterRadius = 0.05f;

    // Whether objects block this light. Costs six extra drawings of the scene per frame (one per cube face); when more
    // lights want shadows than there is room for, the most important ones get them.
    bool castsShadows = true;

    // When there are more lights than seats or shadows, a higher priority always wins over a lower one; brightness at
    // the focus only decides between equals. For lights a game must never lose while they are in view.
    int priority = 0;

    // A light that moves quickly (a flashlight, a car's headlights): global illumination keeps its bounce light apart,
    // with a memory of a fraction of a second, so the bounce follows it instead of trailing behind for seconds (render/
    // gpu/ddgi.h, GlobalIlluminationSettings::fastIrradianceTimeConstant). Costs more probe rays near it.
    bool fast = false;
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

    // How big the glowing opening really is, for the softness of its shadows, like PointLight::emitterRadius.
    float emitterRadius = 0.025f;

    // Whether objects block this light. Costs one extra drawing of the scene per frame.
    bool castsShadows = true;

    // A higher priority always wins a seat or a shadow over a lower one, like PointLight::priority.
    int priority = 0;

    // Its bounce light follows it closely, like PointLight::fast.
    bool fast = false;
};

// Component: light that reaches every surface equally, standing in for light bounced around the scene. Several add up.
struct AmbientLight {
    Vec3 colour{0.2f, 0.2f, 0.2f};
};

// Component: what shows where nothing is drawn, a flat colour standing in for the sky (linear, like light, and may pass
// 1). It lights nothing. Several add up; with none the background is black.
struct SkyBackground {
    Vec3 colour{0.0f, 0.0f, 0.0f};
};
