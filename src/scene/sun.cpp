#include "scene/sun.h"

#include "math/angles.h"

#include <algorithm>
#include <cmath>

namespace {
    // The December solstice, when the sun stands furthest south: the cosine's turning point.
    constexpr float decemberSolsticeDay = 355.0f;
}

/*
* Earth's orbit is nearly a circle, so the declination follows the tilt times a cosine of the year's phase, lowest at the
* December solstice. Counting the phase from day 355 rather than from January 1 keeps it exact at that solstice.
*/
float solarDeclination(float dayOfYear) {
    const float phase = fullTurnRadians * (dayOfYear - decemberSolsticeDay) / daysPerYear;
    return -radiansFromDegrees(earthAxialTiltDegrees) * std::cos(phase);
}

float solarHourAngle(float solarHours) {
    return fullTurnRadians * (solarHours - 12.0f) / hoursPerDay;
}

/*
* The sun's direction in Earth's own frame, turned into the observer's local east, north and up. With the observer's
* latitude phi, the sun's declination delta and the hour angle H:
*   east  = -cos(delta) sin(H)                                  (west in the afternoon, when H > 0)
*   north =  sin(delta) cos(phi) - cos(delta) cos(H) sin(phi)
*   up    =  sin(delta) sin(phi) + cos(delta) cos(H) cos(phi)    (the sine of the elevation)
* The squares add up to 1, so it is a unit vector by construction. The engine's world has +x east, +y up and -z north.
*/
Vec3 directionTowardsSun(float latitude, float declination, float hourAngle) {
    const float sinLatitude = std::sin(latitude);
    const float cosLatitude = std::cos(latitude);
    const float sinDeclination = std::sin(declination);
    const float cosDeclination = std::cos(declination);
    const float cosHour = std::cos(hourAngle);

    const float east = -cosDeclination * std::sin(hourAngle);
    const float north = sinDeclination * cosLatitude - cosDeclination * cosHour * sinLatitude;
    const float up = sinDeclination * sinLatitude + cosDeclination * cosHour * cosLatitude;
    return Vec3{east, up, -north};
}

Vec3 directionTowardsSunOn(float latitudeDegrees, float dayOfYear, float solarHours) {
    return directionTowardsSun(radiansFromDegrees(latitudeDegrees), solarDeclination(dayOfYear), solarHourAngle(solarHours));
}

float sunElevation(const Vec3& towardsSun) {
    return std::asin(std::clamp(towardsSun.y, -1.0f, 1.0f));
}
