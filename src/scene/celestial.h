#pragma once

#include "math/mat4.h"
#include "math/vec3.h"

/*
* The sky beyond the sun: where the moon and the stars stand for an observer, from the date and the time. Stars are fixed
* on the celestial sphere, named by right ascension (its longitude, counted east from where the sun crosses the equator
* in March) and declination (its latitude). That sphere seems to turn once a sidereal day (23 h 56 min) about the
* celestial pole, which stands as high above the north horizon as the observer's latitude.
*
* Positions use the equatorial frame, fixed to the stars: x towards right ascension 0 (the March equinox), z the north
* celestial pole, y completing a right-handed frame (right ascension 90 degrees). The moon and the sun move along the
* ecliptic, the sun's yearly path, tilted earthAxialTiltDegrees from the equator; they are found there by ecliptic
* longitude and latitude and turned into the equatorial frame.
*
* Times are days since J2000 (2000 January 1, 12:00 Universal Time), the epoch of the catalogue's positions and of the
* orbital elements. The light lab's solar time stands in for Universal Time, as if it were on the Greenwich meridian;
* precession (the stars drift about 0.36 degrees in 26 years) and the moon's parallax (up to a degree) are left out.
*/

/*
* Days since J2000 at a moment in the Gregorian calendar: the year, the day of the year (1 = January 1, fractions run
* on) and the hours past midnight. Leap days of the years before it count; the day of the year itself is counted as
* given, so day 60 is March 1 in common years and February 29 in leap years.
*/
double daysSinceJ2000(int year, float dayOfYear, float hours);

// The unit vector in the equatorial frame of a point at ecliptic longitude and latitude (radians).
Vec3 eclipticToEquatorial(double longitude, double latitude);

// The unit vector in the equatorial frame at a right ascension and declination (radians).
Vec3 equatorialDirection(double rightAscension, double declination);

/*
* The sun's ecliptic longitude (radians, 0 to 2 pi) on a day since J2000, from its mean orbit plus the equation of the
* centre (the Astronomical Almanac's low-precision formula, good to 0.01 degrees until 2050).
*/
double sunEclipticLongitude(double days);

/*
* The local sidereal angle (radians): how far the sky has turned, the hour angle of right ascension 0. Any body's hour
* angle is this minus its right ascension, so with the sun's solar hour angle known it is the sun's hour angle plus the
* sun's right ascension. Measured from the sun this way, the stars come round 3 min 56 s earlier each day: Earth turns
* once more a year than there are days, since it also goes once round the sun.
*/
double siderealAngle(double days, float solarHours);

/*
* The rotation from the equatorial frame to the engine's world (+x east, +y up, -z north) for an observer at latitude
* (radians) when the sky has turned by siderealAngle: a body at right ascension a and declination d goes to
* directionTowardsSun(latitude, d, siderealAngle - a). A rotation only (no translation), so it turns every star at once.
*/
Mat4 equatorialToWorld(float latitude, double siderealAngle);
