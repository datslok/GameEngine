#pragma once

#include "math/vec3.h"

/*
* The moon: where it stands among the stars and how much light it sends, by the phase. It circles Earth once a sidereal
* month (27.32 days) on an orbit tilted about 5 degrees from the ecliptic, and comes back to the same phase every 29.53
* days, longer since Earth has meanwhile moved along its own orbit. Its position comes from mean orbital elements plus
* the largest periodic terms (Meeus, "Astronomical Algorithms", chapter 47, truncated), good to about 0.3 degrees.
*/

// Half the angle the moon's disc spans from Earth, on average (0.518 degrees across). Like the sun's, so it shades as
// softly.
inline constexpr float moonAngularRadius = 0.00452f;

struct EclipticPosition {
    double longitude = 0.0; // radians
    double latitude = 0.0;  // radians
};

// The moon's ecliptic longitude (0 to 2 pi) and latitude on a day since J2000 (scene/celestial.h).
EclipticPosition moonEclipticPosition(double days);

/*
* The phase angle: the angle at the moon between the sun and Earth, from the directions towards each seen from Earth.
* The sun is 400 times further away, so that is pi minus the angle between them in the sky: 0 at full moon, pi at new
* moon, pi / 2 at the quarters.
*/
float moonPhaseAngle(const Vec3& towardsSun, const Vec3& towardsMoon);

/*
* The moon's light against the full moon's at a phase angle (radians), from its magnitude's measured phase curve,
* 0.026 |alpha| + 4e-9 alpha^4 magnitudes fainter (alpha in degrees; Allen's Astrophysical Quantities). Far steeper than
* the lit share of the disc: a half moon gives about 9% of the full moon's light, not half, since at full moon every
* crater's shadow hides behind the rock that casts it (the opposition surge) and away from it the rough ground shades
* itself.
*/
float moonPhaseBrightness(float phaseAngle);
