#include "render/gpu/reflection_probe.h"
#include "math/smooth_step.h"
#include "render/gpu/brdf.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/mip_levels.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

/*
* Vulkan's table of cube faces: for a direction r whose largest component picks the face, s = (sc / |ma| + 1) / 2 and
* t = (tc / |ma| + 1) / 2, with +X: sc = -rz, tc = -ry; -X: sc = rz, tc = -ry; +Y: sc = rx, tc = rz; -Y: sc = rx,
* tc = -rz; +Z: sc = rx, tc = -ry; -Z: sc = -rx, tc = -ry. So s grows along right = d(sc)/dr and t along d(tc)/dr, and
* up is the opposite of the latter.
*/
CubeFaceAxes cubeFaceAxes(int face) {
    switch (face) {
    case 0:
        return {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}};
    case 1:
        return {Vec3{-1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 1.0f, 0.0f}};
    case 2:
        return {Vec3{0.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}};
    case 3:
        return {Vec3{0.0f, -1.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
    case 4:
        return {Vec3{0.0f, 0.0f, 1.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}};
    case 5:
        return {Vec3{0.0f, 0.0f, -1.0f}, Vec3{-1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}};
    default:
        throw std::invalid_argument("A cube has faces 0 to 5");
    }
}

// The face of the largest component (x before y before z on a tie, as the specification's order lists them).
CubeTexel cubeTexelOf(const Vec3& direction) {
    const float ax = std::fabs(direction.x);
    const float ay = std::fabs(direction.y);
    const float az = std::fabs(direction.z);
    int face = 0;

    if (ax >= ay && ax >= az) {
        face = direction.x >= 0.0f ? 0 : 1;
    } else if (ay >= az) {
        face = direction.y >= 0.0f ? 2 : 3;
    } else {
        face = direction.z >= 0.0f ? 4 : 5;
    }

    const CubeFaceAxes axes = cubeFaceAxes(face);
    const float major = axes.forward.dot(direction);
    const float sc = axes.right.dot(direction) / major;
    const float tc = -axes.up.dot(direction) / major;
    return CubeTexel{face, Vec2{0.5f * (sc + 1.0f), 0.5f * (tc + 1.0f)}};
}

Vec3 cubeTexelDirection(int face, Vec2 st) {
    const CubeFaceAxes axes = cubeFaceAxes(face);
    return axes.forward + axes.right * (2.0f * st.x - 1.0f) - axes.up * (2.0f * st.y - 1.0f);
}

/*
* A camera at the origin whose view rows are right, up and minus forward (looking down its own -z, as the engine's
* cameras do) with a square 90-degree lens: clip x = right . p, clip y = up . p and w = forward . p, exactly the cube's
* sc, -tc and ma. Right, up and forward make a left-handed set, hence the mirror image.
*/
Mat4 cubeFaceViewProjection(int face, float nearPlane, float farPlane) {
    const CubeFaceAxes axes = cubeFaceAxes(face);
    Mat4 view = Mat4::identity();
    const Vec3 rows[3] = {axes.right, axes.up, axes.forward * -1.0f};

    for (int row = 0; row < 3; ++row) {
        view.values[row][0] = rows[row].x;
        view.values[row][1] = rows[row].y;
        view.values[row][2] = rows[row].z;
    }

    return toGpuDepthRange(Mat4::perspective(0.5f * std::numbers::pi_v<float>, 1.0f, nearPlane, farPlane)) * view;
}

// The 90-degree lens reaches as far sideways as forward (tan 45 = 1), so the rays' right and up are the face's own.
GpuRayDebugView packCubeFaceRays(int face, const Vec3& origin, std::uint32_t size) {
    const CubeFaceAxes axes = cubeFaceAxes(face);
    GpuRayDebugView rays{};
    const auto write = [](float* target, const Vec3& value) {
        target[0] = value.x;
        target[1] = value.y;
        target[2] = value.z;
    };

    write(rays.right, axes.right);
    write(rays.up, axes.up);
    write(rays.forward, axes.forward);
    write(rays.originAndView, origin);
    rays.screen[0] = static_cast<float>(size);
    rays.screen[1] = static_cast<float>(size);
    return rays;
}

float probeRadialDistance(float viewDistance, const Vec3& unitDirection) {
    const float major = std::max({std::fabs(unitDirection.x), std::fabs(unitDirection.y), std::fabs(unitDirection.z)});
    return viewDistance / major;
}

/*
* |p + d t|^2 = D^2 is t^2 + 2 (p.d) t + |p|^2 - D^2 = 0 for a unit d: the larger root, t = -(p.d) + sqrt((p.d)^2 -
* |p|^2 + D^2), is where the ray leaves the sphere of radius D round the probe. None if the ray never reaches D (the
* sphere misses it) or reaches it only behind its start.
*/
float parallaxStep(const Vec3& position, const Vec3& direction, float storedDistance) {
    const float along = position.dot(direction);
    const float discriminant = along * along - position.dot(position) + storedDistance * storedDistance;

    if (discriminant < 0.0f) {
        return -1.0f;
    }

    const float t = -along + std::sqrt(discriminant);
    return t >= 0.0f ? t : -1.0f;
}

Vec3 parallaxDirection(const Vec3& position, const Vec3& direction, float t) {
    return t >= 0.0f ? (position + direction * t).normalized() : direction;
}

float parallaxConfidence(float reached, float stored) {
    const float mismatch = std::fabs(reached - stored) / std::max(stored, 1e-6f);
    return 1.0f - smoothStep(static_cast<float>(PROBE_MATCH_SHARE), static_cast<float>(PROBE_MISMATCH_SHARE), mismatch);
}

/*
* Each step reads the distance the probe saw towards where the ray now is and moves the ray's point onto that distance:
* a fixed-point search that settles where the two agree. Where the ray passes something the probe could not see, they
* never agree, and the confidence says so.
*/
ParallaxLookup searchProbe(const Vec3& position, const Vec3& direction, const std::function<float(const Vec3&)>& distanceAt) {
    float t = parallaxStep(position, direction, distanceAt(direction));

    for (int step = 0; step < PROBE_SEARCH_STEPS && t >= 0.0f; ++step) {
        t = parallaxStep(position, direction, distanceAt(parallaxDirection(position, direction, t)));
    }

    if (t < 0.0f) {
        return ParallaxLookup{direction, 0.0f};
    }

    const Vec3 found = parallaxDirection(position, direction, t);
    return ParallaxLookup{found, parallaxConfidence((position + direction * t).length(), distanceAt(found))};
}

std::uint32_t probeLevels(std::uint32_t size) {
    const std::uint32_t full = mipLevelCount(size, size);
    const std::uint32_t smallest = mipLevelCount(PROBE_SMALLEST_LEVEL_SIZE, PROBE_SMALLEST_LEVEL_SIZE);
    return full > smallest ? full - smallest + 1 : 1;
}

float probeLevelRoughness(std::uint32_t level, std::uint32_t levels) {
    return levels > 1 ? static_cast<float>(level) / static_cast<float>(levels - 1) : 0.0f;
}

float probeRoughnessLevel(float roughness, std::uint32_t levels) {
    return std::clamp(roughness, 0.0f, 1.0f) * static_cast<float>(levels - 1);
}

float prefilterSourceLevel(float pdf, std::uint32_t samples, std::uint32_t size) {
    const float sampleAngle = 1.0f / (static_cast<float>(samples) * pdf);
    const float texelAngle = 4.0f * std::numbers::pi_v<float> / (6.0f * static_cast<float>(size) * static_cast<float>(size));
    return std::max(0.5f * std::log2(sampleAngle / texelAngle) + 1.0f, 0.0f);
}

/*
* With the normal and the view both along the direction, a facet normal h reflects the view into l = 2 (v.h) h - v,
* drawn with density D (n.h) / (4 v.h) = D / 4. A mirror (roughness 0) reads the sharp top level.
*/
Vec3 prefilterTexel(const Vec3& direction, float roughness, std::uint32_t samples, std::uint32_t sourceSize,
                    const std::function<Vec3(const Vec3&, float)>& sample) {
    const float alpha = roughness * roughness;
    Vec3 sum;
    float weight = 0.0f;

    for (std::uint32_t index = 0; index < samples; ++index) {
        const Vec3 halfway = alongNormal(ggxSampleHalfway(index, samples, alpha), direction);
        const float vDotH = direction.dot(halfway);
        const Vec3 toLight = halfway * (2.0f * vDotH) - direction;
        const float nDotL = direction.dot(toLight);

        if (nDotL <= 0.0f) {
            continue;
        }

        const float level = roughness > 0.0f ? prefilterSourceLevel(ggxDistribution(vDotH, alpha) * 0.25f, samples, sourceSize) : 0.0f;
        sum = sum + sample(toLight, level) * nDotL;
        weight += nDotL;
    }

    return weight > 0.0f ? sum * (1.0f / weight) : Vec3{};
}

Vec3 blendedStandIn(const Vec3& standIn, const Vec3& specularAlbedo, const Vec3& light, float confidence) {
    return standIn + (specularAlbedo * light - standIn) * confidence;
}

GpuPrefilterPass packPrefilterPass(std::uint32_t size, std::uint32_t level, std::uint32_t levels, std::uint32_t sourceSize,
                                   std::uint32_t sourceLevels) {
    GpuPrefilterPass pass{};
    pass.target[0] = static_cast<float>(size);
    pass.target[1] = static_cast<float>(level);
    pass.target[2] = probeLevelRoughness(level, levels);
    pass.source[0] = static_cast<float>(sourceSize);
    pass.source[1] = static_cast<float>(sourceLevels);
    return pass;
}
