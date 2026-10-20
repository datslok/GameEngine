#include "scene/moon.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
    constexpr double pi = std::numbers::pi;

    double radians(double degrees) {
        return degrees * pi / 180.0;
    }

    double sinDegrees(double degrees) {
        return std::sin(radians(degrees));
    }
}

/*
* The moon's mean longitude L', its mean elongation from the sun D, the sun's mean anomaly M, the moon's mean anomaly M'
* and its argument of latitude F (its distance along the orbit from where it crosses the ecliptic northwards), all in
* degrees and advancing linearly with time. The largest terms of the series (Meeus, chapter 47) correct them: the
* equation of the centre (6.289 sin M'), the evection (1.274), the variation (0.658) and smaller ones. Doubles, since
* after 9,000 days the angles run to a hundred thousand degrees.
*/
EclipticPosition moonEclipticPosition(double days) {
    const double meanLongitude = 218.3165 + 13.17639648 * days;
    const double elongation = 297.8502 + 12.19074912 * days;
    const double sunAnomaly = 357.5291 + 0.98560028 * days;
    const double moonAnomaly = 134.9634 + 13.06499295 * days;
    const double latitudeArgument = 93.2721 + 13.22935024 * days;

    const double longitude = meanLongitude + 6.289 * sinDegrees(moonAnomaly) + 1.274 * sinDegrees(2.0 * elongation - moonAnomaly) +
                             0.658 * sinDegrees(2.0 * elongation) + 0.214 * sinDegrees(2.0 * moonAnomaly) - 0.186 * sinDegrees(sunAnomaly) -
                             0.114 * sinDegrees(2.0 * latitudeArgument);
    const double latitude = 5.128 * sinDegrees(latitudeArgument) + 0.281 * sinDegrees(moonAnomaly + latitudeArgument) +
                            0.278 * sinDegrees(moonAnomaly - latitudeArgument) + 0.173 * sinDegrees(2.0 * elongation - latitudeArgument);

    const double turned = std::fmod(radians(longitude), 2.0 * pi);
    return EclipticPosition{turned < 0.0 ? turned + 2.0 * pi : turned, radians(latitude)};
}

float moonPhaseAngle(const Vec3& towardsSun, const Vec3& towardsMoon) {
    const float cosine = std::clamp(towardsSun.normalized().dot(towardsMoon.normalized()), -1.0f, 1.0f);
    return std::numbers::pi_v<float> - std::acos(cosine);
}

float moonPhaseBrightness(float phaseAngle) {
    const float degrees = std::abs(phaseAngle) * 180.0f / std::numbers::pi_v<float>;
    const float fainter = 0.026f * degrees + 4e-9f * degrees * degrees * degrees * degrees;
    return std::pow(10.0f, -0.4f * fainter);
}
