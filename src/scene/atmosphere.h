#pragma once

#include "core/settings_visitor.h"
#include "math/vec3.h"

/*
* Component: a planet's atmosphere, which the renderer draws as the sky and lets light the scene (render/gpu/
* atmosphere.h; Hillaire 2020). A game places one, as it places lights. The sky is lit by the frame's brightest
* directional light (the sun, its value as it is above the atmosphere): every directional light reaching the ground
* is dimmed and reddened by the air it crosses, the sky behind everything is drawn from the light the air scatters
* (with the sun's disc), probe rays that escape bring that sky back, and the flat ambient light gains the light that
* sky gives an open surface. Distances are in kilometres and the coefficients per kilometre, as in the paper; the
* defaults are Earth's. The world's y = 0 stands groundAltitude above the planet's surface.
*/
struct Atmosphere {
    float bottomRadius = 6360.0f; // the planet's surface, km
    float topRadius = 6460.0f;    // where the air ends, km

    // Air molecules (Rayleigh scattering, strongest for blue: 1 / wavelength^4), thinning with height with this scale
    // height: the density falls by e every 8 km.
    Vec3 rayleighScattering{0.005802f, 0.013558f, 0.0331f};
    float rayleighScaleHeight = 8.0f;

    // Aerosols (Mie scattering: dust and droplets, every colour alike, mostly forward), a thinner low layer.
    float mieScattering = 0.003996f;
    float mieAbsorption = 0.0044f;
    float mieScaleHeight = 1.2f;
    float mieAnisotropy = 0.8f; // the Henyey-Greenstein phase function's g: 0 even, towards 1 all forward

    // Ozone absorbs a little orange and red, which keeps the twilight sky blue: a layer whose density rises linearly to
    // 1 at its centre and falls back to 0 half its width either side.
    Vec3 ozoneAbsorption{0.000650f, 0.001881f, 0.000085f};
    float ozoneCentre = 25.0f;
    float ozoneHalfWidth = 15.0f;

    // How much light the planet's surface sends back up into the air (for light scattered more than once).
    Vec3 groundAlbedo{0.3f, 0.3f, 0.3f};

    // How high the world's y = 0 stands above the planet's surface, km.
    float groundAltitude = 0.0f;

    // The sun's disc as drawn in the sky against its real size (the light's angular radius), for looks only: a bigger
    // disc glows dimmer by as much, so it gives the same light. Set by the game with its own disc size (not in visit).
    float sunDiscScale = 1.0f;

    // The same for the moon's disc (DirectionalLight::reflectsSun), drawn with its phase.
    float moonDiscScale = 1.0f;
};

void visit(Atmosphere& atmosphere, SettingsVisitor& visitor);
