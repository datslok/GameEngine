#include "scene/sky_object.h"

#include <algorithm>
#include <cmath>
#include <numbers>

/*
* A sphere of radius r at distance d spans asin(r / d) either side of its centre, so r = d sin(theta).
*/
Transform skyObjectPose(const Vec3& cameraPosition, const Vec3& towardsObject, float distance, float angularRadius) {
    const Vec3 direction = towardsObject.lengthSquared() > 0.0f ? towardsObject.normalized() : Vec3{0.0f, 1.0f, 0.0f};
    const float radius = distance * std::sin(angularRadius);

    Transform pose;
    pose.position = cameraPosition + direction * distance;
    pose.scale = Vec3{radius, radius, radius};
    return pose;
}

// The half-angle form keeps its precision for tiny discs, where 1 - cos(theta) would lose most of its digits.
float discSolidAngle(float angularRadius) {
    const float halfSine = std::sin(angularRadius * 0.5f);
    return 4.0f * std::numbers::pi_v<float> * halfSine * halfSine;
}

Vec3 discRadiance(const Vec3& light, float angularRadius) {
    const float solidAngle = discSolidAngle(angularRadius);

    if (solidAngle <= 0.0f) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }

    const Vec3 radiance = light * (std::numbers::pi_v<float> / solidAngle);
    const float brightest = std::max({radiance.x, radiance.y, radiance.z});
    return brightest > maxSkyRadiance ? radiance * (maxSkyRadiance / brightest) : radiance;
}
