#include "scene/celestial.h"
#include "scene/sun.h"

#include <cmath>
#include <numbers>

namespace {
    constexpr double pi = std::numbers::pi;

    double radians(double degrees) {
        return degrees * pi / 180.0;
    }

    // Days from 1 January of year 1 to 1 January of a year, in the proleptic Gregorian calendar: 365 a year plus a leap
    // day every fourth year, except centuries not divisible by 400.
    long long daysBeforeYear(int year) {
        const long long previous = static_cast<long long>(year) - 1;
        return 365 * previous + previous / 4 - previous / 100 + previous / 400;
    }
}

/*
* J2000 is noon on 2000 January 1, half a day after that day began.
*/
double daysSinceJ2000(int year, float dayOfYear, float hours) {
    const double wholeYears = static_cast<double>(daysBeforeYear(year) - daysBeforeYear(2000));
    return wholeYears + (static_cast<double>(dayOfYear) - 1.0) + static_cast<double>(hours) / 24.0 - 0.5;
}

/*
* Turn the ecliptic's frame about the equinox's direction (x) by the axis's tilt: a point at longitude l and latitude b
* is (cos b cos l, cos b sin l, sin b) in the ecliptic's frame.
*/
Vec3 eclipticToEquatorial(double longitude, double latitude) {
    const double tilt = radians(earthAxialTiltDegrees);
    const double x = std::cos(latitude) * std::cos(longitude);
    const double y = std::cos(latitude) * std::sin(longitude);
    const double z = std::sin(latitude);
    return Vec3{static_cast<float>(x), static_cast<float>(y * std::cos(tilt) - z * std::sin(tilt)),
                static_cast<float>(y * std::sin(tilt) + z * std::cos(tilt))};
}

Vec3 equatorialDirection(double rightAscension, double declination) {
    return Vec3{static_cast<float>(std::cos(declination) * std::cos(rightAscension)), static_cast<float>(std::cos(declination) * std::sin(rightAscension)),
                static_cast<float>(std::sin(declination))};
}

/*
* The mean longitude L and mean anomaly g advance about 0.9856 degrees a day; the orbit's eccentricity shifts the true
* longitude by up to 1.9 degrees (the equation of the centre, 1.915 sin g + 0.020 sin 2g).
*/
double sunEclipticLongitude(double days) {
    const double meanLongitude = radians(280.460 + 0.9856474 * days);
    const double meanAnomaly = radians(357.528 + 0.9856003 * days);
    const double longitude = meanLongitude + radians(1.915) * std::sin(meanAnomaly) + radians(0.020) * std::sin(2.0 * meanAnomaly);
    const double turned = std::fmod(longitude, 2.0 * pi);
    return turned < 0.0 ? turned + 2.0 * pi : turned;
}

/*
* The sun's right ascension from its ecliptic longitude (on the ecliptic, latitude 0): atan2 of the equatorial
* direction's y and x.
*/
double siderealAngle(double days, float solarHours) {
    const Vec3 sun = eclipticToEquatorial(sunEclipticLongitude(days), 0.0);
    const double sunRightAscension = std::atan2(static_cast<double>(sun.y), static_cast<double>(sun.x));
    return static_cast<double>(solarHourAngle(solarHours)) + sunRightAscension;
}

/*
* With the hour angle H = theta - a, cos(d) cos(H) and cos(d) sin(H) are linear in the equatorial direction
* e = (cos d cos a, cos d sin a, sin d): c e.x + s e.y and s e.x - c e.y (c, s the cosine and sine of theta). Put into
* directionTowardsSun's east, up and north (sun.cpp) that is a matrix, rows east, up and south (the world's +z):
*   east  = (-s, c, 0)
*   up    = (c cos(phi), s cos(phi), sin(phi))
*   south = (c sin(phi), s sin(phi), -cos(phi))
* Orthonormal, and east x up = south, so a rotation.
*/
Mat4 equatorialToWorld(float latitude, double siderealAngle) {
    const float c = static_cast<float>(std::cos(siderealAngle));
    const float s = static_cast<float>(std::sin(siderealAngle));
    const float sinLatitude = std::sin(latitude);
    const float cosLatitude = std::cos(latitude);

    Mat4 rotation = Mat4::identity();
    rotation.values[0][0] = -s;
    rotation.values[0][1] = c;
    rotation.values[0][2] = 0.0f;
    rotation.values[1][0] = c * cosLatitude;
    rotation.values[1][1] = s * cosLatitude;
    rotation.values[1][2] = sinLatitude;
    rotation.values[2][0] = c * sinLatitude;
    rotation.values[2][1] = s * sinLatitude;
    rotation.values[2][2] = -cosLatitude;
    return rotation;
}
