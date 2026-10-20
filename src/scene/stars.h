#pragma once

#include "math/mat4.h"
#include "math/vec3.h"

#include <memory>
#include <vector>

/*
* The stars: a catalogue's entries as the renderer draws them, and the component a game places to show them. Each star
* is a point of light far smaller than a pixel, fixed on the celestial sphere (scene/celestial.h), with a brightness
* from its magnitude and a colour from its temperature.
*/

// A star as a catalogue lists it.
struct CatalogueStar {
    float rightAscension = 0.0f; // radians, J2000
    float declination = 0.0f;    // radians, J2000
    float magnitude = 0.0f;      // visual (V): 5 magnitudes fainter is 100 times less light
    float colourIndex = 0.0f;    // B - V: how much fainter through a blue filter than a yellow one; bluer stars lower
};

// A star ready to draw: its direction in the equatorial frame, its magnitude, and its colour in linear light with a
// luminance of 1 (the brightness comes from the magnitude).
struct Star {
    Vec3 direction{0.0f, 0.0f, 1.0f};
    float magnitude = 0.0f;
    Vec3 colour{1.0f, 1.0f, 1.0f};
};

/*
* A star's temperature (kelvin) from its colour index, treating it as a blackbody (Ballesteros 2012):
* T = 4600 (1 / (0.92 (B - V) + 1.7) + 1 / (0.92 (B - V) + 0.62)). The sun's 0.65 gives about 5780 K.
*/
float starTemperature(float colourIndex);

/*
* The colour a blackbody at a temperature (kelvin) shows, in linear sRGB with a luminance of 1: Planck's law integrated
* against the CIE 1931 colour matching functions (Wyman, Sloan and Shirley's 2013 fit) into XYZ, then into sRGB's
* primaries, negatives (hues outside sRGB) clipped to 0. About white at 6500 K, orange below, blue above.
*/
Vec3 blackbodyColour(float temperature);

// A catalogue star as drawn: its direction, magnitude and colour.
Star makeStar(const CatalogueStar& star);

/*
* Component: a starry sky, drawn behind the sky and dimmed and reddened by the air like the sun (the renderer draws it
* only with an Atmosphere). The stars are shared, not copied, since the frame takes a copy of the component every
* frame; the game turns them with the sky each frame (equatorialToWorld).
*/
struct StarrySky {
    std::shared_ptr<const std::vector<Star>> stars;

    // From the equatorial frame to the world (scene/celestial.h).
    Mat4 toWorld = Mat4::identity();

    /*
    * The light (the engine's light value, as a directional light's) a star of magnitude 0 gives, and how far magnitudes
    * spread it: each magnitude m gives brightness 10^(-0.4 contrast m), so contrast 1 is the real spread (Sirius 10,000
    * times the faintest the eye sees) and below 1 squeezes it, so faint stars show without the bright ones blinding.
    */
    float brightness = 1.0f;
    float contrast = 1.0f;
};
