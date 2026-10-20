#include "math/angles.h"
#include "scene/celestial.h"
#include "scene/moon.h"
#include "scene/sun.h"

#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <numbers>

namespace {
    constexpr double pi = std::numbers::pi;

    bool nearVector(const Vec3& actual, const Vec3& expected, float within) {
        return near(actual.x, expected.x, within) && near(actual.y, expected.y, within) && near(actual.z, expected.z, within);
    }

    // The difference between two angles (radians), wrapped to -pi..pi.
    double angleBetween(double a, double b) {
        return std::remainder(a - b, 2.0 * pi);
    }

    double degrees(double radians) {
        return radians * 180.0 / pi;
    }

    // J2000 is 2000 January 1 at noon; 2000 was a leap year, so 2001 starts 366 days after its January 1; 2026 starts
    // 26 years of 365 days and the 7 leap days of 2000 to 2024 after 2000's.
    void testDaysSinceJ2000() {
        assert(daysSinceJ2000(2000, 1.0f, 12.0f) == 0.0);
        assert(daysSinceJ2000(2000, 1.0f, 0.0f) == -0.5);
        assert(daysSinceJ2000(2001, 1.0f, 12.0f) == 366.0);
        assert(daysSinceJ2000(2026, 1.0f, 0.0f) == 9496.5);
        assert(daysSinceJ2000(2026, 80.0f, 6.0f) == 9496.5 + 79.25);
        assert(daysSinceJ2000(1999, 365.0f, 12.0f) == -1.0);
    }

    // On the ecliptic at longitude 0 a point is at the March equinox; at 90 degrees it is tilted north by the axis's
    // tilt; the ecliptic's own pole is tilted from the celestial pole by the same angle, away from right ascension 90.
    void testEclipticToEquatorial() {
        const double tilt = radiansFromDegrees(earthAxialTiltDegrees);
        assert(nearVector(eclipticToEquatorial(0.0, 0.0), Vec3{1.0f, 0.0f, 0.0f}, 1e-6f));
        assert(nearVector(eclipticToEquatorial(pi / 2.0, 0.0),
                          Vec3{0.0f, static_cast<float>(std::cos(tilt)), static_cast<float>(std::sin(tilt))}, 1e-6f));
        assert(nearVector(eclipticToEquatorial(0.0, pi / 2.0),
                          Vec3{0.0f, static_cast<float>(-std::sin(tilt)), static_cast<float>(std::cos(tilt))}, 1e-6f));

        assert(nearVector(equatorialDirection(0.0, 0.0), Vec3{1.0f, 0.0f, 0.0f}, 1e-6f));
        assert(nearVector(equatorialDirection(pi / 2.0, 0.0), Vec3{0.0f, 1.0f, 0.0f}, 1e-6f));
        assert(nearVector(equatorialDirection(1.0, pi / 2.0), Vec3{0.0f, 0.0f, 1.0f}, 1e-6f));
    }

    // The sun crosses the equator northwards at the March equinox (2026 March 20, 14:46 UT, day 79) and stands furthest
    // north at the June solstice (June 21, 08:24 UT, day 172).
    void testSunLongitude() {
        const double equinox = sunEclipticLongitude(daysSinceJ2000(2026, 79.0f, 14.77f));
        const double solstice = sunEclipticLongitude(daysSinceJ2000(2026, 172.0f, 8.4f));
        assert(std::abs(degrees(angleBetween(equinox, 0.0))) < 0.05);
        assert(std::abs(degrees(angleBetween(solstice, pi / 2.0))) < 0.05);
    }

    /*
    * A body at right ascension a and declination d stands where the sun would at the hour angle siderealAngle - a and
    * declination d, for any place and time; the rotation keeps lengths.
    */
    void testEquatorialToWorld() {
        for (float latitudeDegrees = -80.0f; latitudeDegrees <= 80.0f; latitudeDegrees += 20.0f) {
            for (double sidereal = 0.0; sidereal < 2.0 * pi; sidereal += 0.7) {
                const Mat4 toWorld = equatorialToWorld(radiansFromDegrees(latitudeDegrees), sidereal);

                for (double rightAscension = 0.1; rightAscension < 2.0 * pi; rightAscension += 0.9) {
                    for (const double declination : {-1.2, -0.3, 0.0, 0.5, 1.4}) {
                        const Vec3 world = transformDirection(toWorld, equatorialDirection(rightAscension, declination));
                        const Vec3 expected = directionTowardsSun(radiansFromDegrees(latitudeDegrees), static_cast<float>(declination),
                                                                  static_cast<float>(sidereal - rightAscension));
                        assert(nearVector(world, expected, 2e-5f));
                    }
                }
            }
        }
    }

    // The celestial pole (Polaris, within a degree of it) stands due north, as high as the latitude, at every hour.
    void testThePoleStandsStill() {
        const Vec3 polaris = equatorialDirection(radiansFromDegrees(37.95f), radiansFromDegrees(89.26f));

        for (float hours = 0.0f; hours < hoursPerDay; hours += 3.0f) {
            const double days = daysSinceJ2000(2026, 80.0f, hours);
            const Vec3 world = transformDirection(equatorialToWorld(radiansFromDegrees(45.0f), siderealAngle(days, hours)), polaris);
            assert(std::abs(degrees(sunElevation(world)) - 45.0) < 0.8);
            assert(world.z < -0.6f);
        }
    }

    /*
    * The sky turns once a sidereal day: at solar noon on the March equinox the sun is at right ascension 0 and on the
    * meridian, so the sidereal angle is near 0; at the same solar time a day later the stars have come round a day's
    * share of a year further (they rise about 4 minutes earlier). Against Greenwich mean sidereal time at the start of
    * 2026 (100.66 degrees at 0 h UT) it differs only by the equation of time, under 4.5 degrees.
    */
    void testSiderealAngle() {
        const double equinoxNoon = daysSinceJ2000(2026, 79.0f, 12.0f);
        assert(std::abs(degrees(angleBetween(siderealAngle(equinoxNoon, 12.0f), 0.0))) < 1.0);

        double gained = 0.0;
        for (float day = 1.0f; day < 366.0f; day += 1.0f) {
            gained += angleBetween(siderealAngle(daysSinceJ2000(2026, day + 1.0f, 21.0f), 21.0f), siderealAngle(daysSinceJ2000(2026, day, 21.0f), 21.0f));
        }
        assert(std::abs(gained / 365.0 - 2.0 * pi / 365.25) < 1e-5);

        const double newYear = daysSinceJ2000(2026, 1.0f, 0.0f);
        assert(std::abs(degrees(angleBetween(siderealAngle(newYear, 0.0f), radiansFromDegrees(100.66)))) < 4.5);
    }

    /*
    * The moon against eclipses, when it lines up with the sun: full at the total lunar eclipse of 2026 March 3 (11:33 UT,
    * day 62), opposite the sun and within half a degree of the ecliptic; new at the annular solar eclipse of February 17
    * (12:01 UT, day 48), beside it. Between, it runs a little over 13 degrees a day eastwards.
    */
    void testMoonAgainstEclipses() {
        const double fullDay = daysSinceJ2000(2026, 62.0f, 11.55f);
        const EclipticPosition full = moonEclipticPosition(fullDay);
        assert(std::abs(degrees(angleBetween(full.longitude, sunEclipticLongitude(fullDay) + pi))) < 0.5);
        assert(std::abs(degrees(full.latitude)) < 0.6);

        const double newDay = daysSinceJ2000(2026, 48.0f, 12.02f);
        const EclipticPosition fresh = moonEclipticPosition(newDay);
        assert(std::abs(degrees(angleBetween(fresh.longitude, sunEclipticLongitude(newDay)))) < 0.5);

        const double step = degrees(angleBetween(moonEclipticPosition(fullDay + 1.0).longitude, full.longitude));
        assert(step > 11.0 && step < 16.0);

        for (double day = 9000.0; day < 9800.0; day += 13.7) {
            assert(std::abs(degrees(moonEclipticPosition(day).latitude)) < 5.9);
        }
    }

    // The phase angle is 0 with the sun behind the viewer, a right angle at the quarters, pi beside the sun.
    void testPhaseAngle() {
        const Vec3 towardsMoon{0.0f, 0.6f, -0.8f};
        assert(near(moonPhaseAngle(towardsMoon * -1.0f, towardsMoon), 0.0f, 1e-3f));
        assert(near(moonPhaseAngle(towardsMoon, towardsMoon), static_cast<float>(pi), 1e-3f));
        assert(near(moonPhaseAngle(Vec3{1.0f, 0.0f, 0.0f}, towardsMoon), static_cast<float>(pi / 2.0), 1e-5f));
    }

    // The full moon is 1; a quarter about 9% (2.6 magnitudes fainter), falling all the way to nearly nothing at new moon.
    void testPhaseBrightness() {
        assert(moonPhaseBrightness(0.0f) == 1.0f);
        assert(near(moonPhaseBrightness(static_cast<float>(pi / 2.0)), 0.091f, 0.005f));
        assert(moonPhaseBrightness(static_cast<float>(pi)) < 1e-3f);

        float previous = 2.0f;
        for (float angle = 0.0f; angle <= static_cast<float>(pi); angle += 0.05f) {
            const float brightness = moonPhaseBrightness(angle);
            assert(brightness < previous && brightness > 0.0f);
            assert(moonPhaseBrightness(-angle) == brightness);
            previous = brightness;
        }
    }
}

void testCelestial() {
    testDaysSinceJ2000();
    testEclipticToEquatorial();
    testSunLongitude();
    testEquatorialToWorld();
    testThePoleStandsStill();
    testSiderealAngle();
    testMoonAgainstEclipses();
    testPhaseAngle();
    testPhaseBrightness();
}
