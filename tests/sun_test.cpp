#include "math/angles.h"
#include "scene/sun.h"

#include "test_levels.h"

#include <cassert>
#include <cmath>

namespace {
    // Directions are unit vectors computed in float, so a few ulps either way.
    constexpr float tolerance = 1e-5f;

    bool nearVector(const Vec3& actual, const Vec3& expected, float within) {
        return near(actual.x, expected.x, within) && near(actual.y, expected.y, within) && near(actual.z, expected.z, within);
    }

    // The declination swings between the tilt's two signs at the solstices and crosses zero near the equinoxes.
    void testDeclinationOverTheYear() {
        const float tilt = radiansFromDegrees(earthAxialTiltDegrees);
        const float oneDegree = radiansFromDegrees(1.0f);

        assert(near(solarDeclination(355.0f), -tilt, 1e-4f));
        assert(near(solarDeclination(172.0f), tilt, oneDegree));
        assert(near(solarDeclination(80.0f), 0.0f, oneDegree));
        assert(near(solarDeclination(266.0f), 0.0f, oneDegree));

        for (float day = 1.0f; day <= daysPerYear; day += 7.0f) {
            assert(std::abs(solarDeclination(day)) <= tilt + tolerance);
        }
    }

    // Earth turns 15 degrees an hour: noon is 0, the afternoon positive.
    void testHourAngle() {
        assert(near(solarHourAngle(12.0f), 0.0f, tolerance));
        assert(near(solarHourAngle(18.0f), radiansFromDegrees(90.0f), tolerance));
        assert(near(solarHourAngle(6.0f), radiansFromDegrees(-90.0f), tolerance));
        assert(near(solarHourAngle(13.0f), radiansFromDegrees(15.0f), tolerance));
    }

    // Whatever the place, day and time, the direction is a unit vector.
    void testDirectionIsUnit() {
        for (float latitude = -90.0f; latitude <= 90.0f; latitude += 15.0f) {
            for (float day = 1.0f; day <= daysPerYear; day += 30.0f) {
                for (float hours = 0.0f; hours < hoursPerDay; hours += 1.5f) {
                    assert(near(directionTowardsSunOn(latitude, day, hours).length(), 1.0f, tolerance));
                }
            }
        }
    }

    /*
    * At solar noon the sun stands on the meridian, 90 - latitude + declination degrees up: due south of an observer north
    * of the sun's latitude (+z is south), due north of one south of it, straight overhead where they are equal.
    */
    void testNoon() {
        const float noon = solarHourAngle(12.0f);

        for (const float latitudeDegrees : {60.0f, 45.0f, 10.0f, -30.0f}) {
            for (const float declinationDegrees : {-23.0f, 0.0f, 23.0f}) {
                const Vec3 sun = directionTowardsSun(radiansFromDegrees(latitudeDegrees), radiansFromDegrees(declinationDegrees), noon);
                const float expected = radiansFromDegrees(90.0f - std::abs(latitudeDegrees - declinationDegrees));

                assert(near(sunElevation(sun), expected, 1e-4f));
                assert(near(sun.x, 0.0f, tolerance));
                assert((latitudeDegrees > declinationDegrees) == (sun.z > 0.0f));
            }
        }

        const Vec3 overhead = directionTowardsSun(0.0f, 0.0f, noon);
        assert(nearVector(overhead, Vec3{0.0f, 1.0f, 0.0f}, tolerance));
    }

    // The sun rises in the east (+x) and sets in the west, and the afternoon mirrors the morning across the meridian.
    void testMorningMirrorsAfternoon() {
        const float latitude = radiansFromDegrees(45.0f);
        const float declination = radiansFromDegrees(10.0f);

        for (float hours = 0.5f; hours < 12.0f; hours += 0.5f) {
            const Vec3 morning = directionTowardsSun(latitude, declination, solarHourAngle(12.0f - hours));
            const Vec3 afternoon = directionTowardsSun(latitude, declination, solarHourAngle(12.0f + hours));

            assert(morning.x > 0.0f);
            assert(afternoon.x < 0.0f);
            assert(near(morning.x, -afternoon.x, tolerance));
            assert(near(morning.y, afternoon.y, tolerance));
            assert(near(morning.z, afternoon.z, tolerance));
        }
    }

    /*
    * Sunrise and sunset fall where the elevation is zero, at the hour angle with cos H = -tan(latitude) tan(declination);
    * at an equinox that is 6 and 18 everywhere, and midnight is dark away from the poles.
    */
    void testSunriseAndSunset() {
        assert(near(directionTowardsSun(radiansFromDegrees(45.0f), 0.0f, solarHourAngle(6.0f)).y, 0.0f, tolerance));
        assert(near(directionTowardsSun(radiansFromDegrees(45.0f), 0.0f, solarHourAngle(18.0f)).y, 0.0f, tolerance));
        assert(directionTowardsSun(radiansFromDegrees(45.0f), 0.0f, solarHourAngle(0.0f)).y < -0.5f);

        const float latitude = radiansFromDegrees(52.0f);
        const float declination = radiansFromDegrees(20.0f);
        const float sunrise = std::acos(-std::tan(latitude) * std::tan(declination));
        assert(near(directionTowardsSun(latitude, declination, sunrise).y, 0.0f, tolerance));
        assert(near(directionTowardsSun(latitude, declination, -sunrise).y, 0.0f, tolerance));
        assert(directionTowardsSun(latitude, declination, sunrise - 0.1f).y > 0.0f);
    }

    // At the north pole in summer the sun circles the sky at the same height all day: the midnight sun.
    void testMidnightSun() {
        const float pole = radiansFromDegrees(90.0f);
        const float declination = radiansFromDegrees(20.0f);

        for (float hours = 0.0f; hours < hoursPerDay; hours += 2.0f) {
            assert(near(sunElevation(directionTowardsSun(pole, declination, solarHourAngle(hours))), declination, 1e-4f));
        }
    }

    // The convenience form agrees with the angles it is built from.
    void testCalendarForm() {
        const Vec3 fromAngles = directionTowardsSun(radiansFromDegrees(45.0f), solarDeclination(80.0f), solarHourAngle(15.5f));
        assert(nearVector(directionTowardsSunOn(45.0f, 80.0f, 15.5f), fromAngles, tolerance));
    }
}

void testSun() {
    testDeclinationOverTheYear();
    testHourAngle();
    testDirectionIsUnit();
    testNoon();
    testMorningMirrorsAfternoon();
    testSunriseAndSunset();
    testMidnightSun();
    testCalendarForm();
}
