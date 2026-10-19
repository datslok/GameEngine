#pragma once

#include "math/vec3.h"

/*
* Where the sun stands in the sky, from where you are on Earth, the day of the year and the time of day: the spherical
* trigonometry of a sundial. Earth turns once a day about an axis tilted 23.44 degrees from its orbit, so the sun seems
* to circle the celestial pole once a day, at an angle from the celestial equator (its declination) that swings
* between -23.44 and +23.44 degrees over the year.
*
* Times are local solar time: 12 is the moment the sun crosses the meridian (due south of a northern observer), which
* differs from clock time by the time zone and by the equation of time (up to about 16 minutes over the year), both
* left out. Atmospheric refraction, which lifts the sun about half a degree at the horizon, is left out too.
*/

// How far Earth's axis is tilted from its orbit, the largest declination the sun reaches.
inline constexpr float earthAxialTiltDegrees = 23.44f;

inline constexpr float hoursPerDay = 24.0f;
inline constexpr float daysPerYear = 365.0f;

// The sun's declination in radians on a day of the year (1 = January 1): -23.44 degrees around December 21 (day 355),
// +23.44 around June 21 (day 172), near 0 at the equinoxes. A cosine through the December solstice: good to about a
// degree, plenty for lighting.
float solarDeclination(float dayOfYear);

// The hour angle in radians: how far Earth has turned since solar noon, 15 degrees an hour, negative in the morning.
float solarHourAngle(float solarHours);

/*
* The unit direction towards the sun in the engine's world (+x east, +y up, -z north) from the observer's latitude (radians,
* positive north), the sun's declination and the hour angle.
*/
Vec3 directionTowardsSun(float latitude, float declination, float hourAngle);

// The same from a latitude in degrees, a day of the year and a local solar time in hours.
Vec3 directionTowardsSunOn(float latitudeDegrees, float dayOfYear, float solarHours);

// The sun's height above the horizon in radians (negative below it), from a unit direction towards it.
float sunElevation(const Vec3& towardsSun);
