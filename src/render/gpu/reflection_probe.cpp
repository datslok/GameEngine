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

float parallaxMismatch(const Vec3& position, const Vec3& direction, float t, float storedDistance) {
    return (position + direction * t).length() - storedDistance;
}

float ProbeDistanceCube::at(const Vec3& direction, std::uint32_t level) const {
    const CubeTexel texel = cubeTexelOf(direction);
    const std::uint32_t side = std::max(size >> level, 1u);
    const std::uint32_t x = std::min(static_cast<std::uint32_t>(std::max(texel.st.x, 0.0f) * static_cast<float>(side)), side - 1);
    const std::uint32_t y = std::min(static_cast<std::uint32_t>(std::max(texel.st.y, 0.0f) * static_cast<float>(side)), side - 1);
    return levels[level][static_cast<std::size_t>(texel.face)][y * side + x];
}

// Texel i's centre is at (i + 0.5) / size, so a point s sits between texels floor(s size - 0.5) and the next.
float ProbeDistanceCube::filtered(const Vec3& direction) const {
    const CubeTexel texel = cubeTexelOf(direction);
    const std::vector<float>& face = levels[0][static_cast<std::size_t>(texel.face)];
    const float x = texel.st.x * static_cast<float>(size) - 0.5f;
    const float y = texel.st.y * static_cast<float>(size) - 0.5f;
    const float x0 = std::floor(x);
    const float y0 = std::floor(y);
    const float fx = x - x0;
    const float fy = y - y0;
    const auto clamped = [&](float value) { return static_cast<std::uint32_t>(std::clamp(value, 0.0f, static_cast<float>(size - 1))); };
    const auto texelAt = [&](float tx, float ty) { return face[clamped(ty) * size + clamped(tx)]; };
    const float top = texelAt(x0, y0) * (1.0f - fx) + texelAt(x0 + 1.0f, y0) * fx;
    const float bottom = texelAt(x0, y0 + 1.0f) * (1.0f - fx) + texelAt(x0 + 1.0f, y0 + 1.0f) * fx;
    return top * (1.0f - fy) + bottom * fy;
}

float ProbeDistanceCube::spread(const Vec3& direction) const {
    const CubeTexel texel = cubeTexelOf(direction);
    const std::vector<float>& face = levels[0][static_cast<std::size_t>(texel.face)];
    const float x0 = std::floor(texel.st.x * static_cast<float>(size) - 0.5f);
    const float y0 = std::floor(texel.st.y * static_cast<float>(size) - 0.5f);
    const auto clamped = [&](float value) { return static_cast<std::uint32_t>(std::clamp(value, 0.0f, static_cast<float>(size - 1))); };
    const auto texelAt = [&](float tx, float ty) { return face[clamped(ty) * size + clamped(tx)]; };
    const float a = texelAt(x0, y0);
    const float b = texelAt(x0 + 1.0f, y0);
    const float c = texelAt(x0, y0 + 1.0f);
    const float d = texelAt(x0 + 1.0f, y0 + 1.0f);
    const float farthest = std::max({a, b, c, d});
    return (farthest - std::min({a, b, c, d})) / std::max(farthest, 1e-6f);
}

ProbeDistanceCube buildProbeDistanceCube(std::uint32_t size, const std::function<float(const Vec3&)>& distance) {
    ProbeDistanceCube cube;
    cube.size = size;
    const std::uint32_t levelCount = mipLevelCount(size, size);
    cube.levels.resize(levelCount);

    for (int face = 0; face < cubeFaceCount; ++face) {
        std::vector<float>& top = cube.levels[0][static_cast<std::size_t>(face)];
        top.resize(static_cast<std::size_t>(size) * size);

        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const Vec2 st{(static_cast<float>(x) + 0.5f) / static_cast<float>(size), (static_cast<float>(y) + 0.5f) / static_cast<float>(size)};
                top[y * size + x] = distance(cubeTexelDirection(face, st).normalized());
            }
        }

        for (std::uint32_t level = 1; level < levelCount; ++level) {
            const std::uint32_t side = std::max(size >> level, 1u);
            const std::uint32_t belowSide = side * 2;
            const std::vector<float>& below = cube.levels[level - 1][static_cast<std::size_t>(face)];
            std::vector<float>& texels = cube.levels[level][static_cast<std::size_t>(face)];
            texels.resize(static_cast<std::size_t>(side) * side);

            for (std::uint32_t y = 0; y < side; ++y) {
                for (std::uint32_t x = 0; x < side; ++x) {
                    texels[y * side + x] = std::min({below[2 * y * belowSide + 2 * x], below[2 * y * belowSide + 2 * x + 1],
                                                     below[(2 * y + 1) * belowSide + 2 * x], below[(2 * y + 1) * belowSide + 2 * x + 1]});
                }
            }
        }
    }

    return cube;
}

float probeTexelAngle(std::uint32_t size) {
    return 0.5f * std::numbers::pi_v<float> / static_cast<float>(size);
}

float probeStartLift(const Vec3& position, const Vec3& normal, std::uint32_t size) {
    const float distance = position.length();
    const float facing = distance > 0.0f ? normal.dot(position * (-1.0f / distance)) : 1.0f;
    const float texels = static_cast<float>(PROBE_START_LIFT_TEXELS) * probeTexelAngle(size) * distance;
    return texels / std::max(facing, static_cast<float>(PROBE_START_LIFT_SLANT));
}

float probeEdgeConfidence(float spread) {
    return 1.0f - smoothStep(static_cast<float>(PROBE_EDGE_JUMP_START), static_cast<float>(PROBE_EDGE_JUMP_END), spread);
}

float probeViewConfidence(float angle, std::uint32_t size) {
    const float texel = probeTexelAngle(size);
    return 1.0f - smoothStep(texel, 2.0f * texel, angle);
}

CubeCell cubeCellOf(const Vec3& direction, std::uint32_t side) {
    const CubeTexel texel = cubeTexelOf(direction);
    const int last = static_cast<int>(side) - 1;
    const int x = std::clamp(static_cast<int>(texel.st.x * static_cast<float>(side)), 0, last);
    const int y = std::clamp(static_cast<int>(texel.st.y * static_cast<float>(side)), 0, last);
    return CubeCell{texel.face, x, y};
}

/*
* On a face, sc = (right . q) / (forward . q), so the points with sc = c make the plane (right - c forward) . q = 0
* through the probe; tc = -(up . q) / (forward . q) likewise makes (-up - c forward) . q = 0. The ray meets each plane
* once, at t = -(position . n) / (direction . n); a crossing behind the face (forward . q < 0) is the plane's far side,
* not this cell's edge.
*/
float cubeCellExit(const Vec3& position, const Vec3& direction, float from, const CubeCell& cell, std::uint32_t side, float end) {
    const CubeFaceAxes axes = cubeFaceAxes(cell.face);
    const float across = static_cast<float>(side);
    const float edges[4] = {2.0f * static_cast<float>(cell.x) / across - 1.0f, 2.0f * static_cast<float>(cell.x + 1) / across - 1.0f,
                            2.0f * static_cast<float>(cell.y) / across - 1.0f, 2.0f * static_cast<float>(cell.y + 1) / across - 1.0f};
    float exit = end;

    for (int edge = 0; edge < 4; ++edge) {
        const Vec3 normal = edge < 2 ? axes.right - axes.forward * edges[edge] : axes.up * -1.0f - axes.forward * edges[edge];
        const float approach = direction.dot(normal);

        if (std::fabs(approach) < 1e-12f) {
            continue;
        }

        const float t = -position.dot(normal) / approach;

        if (t > from && t < exit && axes.forward.dot(position + direction * t) > 0.0f) {
            exit = t;
        }
    }

    if (exit >= end) {
        return end;
    }

    return std::min(exit + std::max(exit * static_cast<float>(PROBE_CELL_CROSSING_SHARE), static_cast<float>(PROBE_CELL_CROSSING)), end);
}

Vec3 parallaxDirection(const Vec3& position, const Vec3& direction, float t) {
    return t >= 0.0f ? (position + direction * t).normalized() : direction;
}

float parallaxConfidence(float nearSide, float farSide) {
    const float mismatch = std::fabs(nearSide - farSide) / std::max(std::max(nearSide, farSide), 1e-6f);
    return 1.0f - smoothStep(static_cast<float>(PROBE_MATCH_SHARE), static_cast<float>(PROBE_MISMATCH_SHARE), mismatch);
}

/*
* The ray meets a surface the first time it goes from in front of what the probe saw to behind it (an equation
* |P + R t| = D(direction) has other roots further on, through doorways and behind walls, which a solver might settle
* on). Near its start the ray may sit a hair behind its own surface, so it first leaves it. Then it walks the pyramid
* as screen-space reflections walk theirs: a cell's nearest distance is a sphere nothing in the cell comes inside, so
* while the ray stays in front of it across the cell (its distance from the probe along a line is greatest at an end)
* it goes on to the next cell, a level coarser; else a level finer without moving. At level 0 the texel's blended
* distance where the ray leaves it says whether it went behind; a ray going far behind the texel's exact distance with
* nothing there to meet has passed behind something the probe saw in front, and cannot be judged. The crossing is
* narrowed down by halving; the probe's distances either side agree for a real surface (and the four texels blended
* there are one surface), and jump at an edge it saw past, which the confidence weighs.
*/
ParallaxLookup searchProbe(const Vec3& position, const Vec3& direction, float far, std::uint32_t size, std::uint32_t levels,
                           const std::function<float(const Vec3&, std::uint32_t)>& cellAt,
                           const std::function<float(const Vec3&)>& filteredAt, const std::function<float(const Vec3&)>& spreadAt) {
    const ParallaxLookup none{direction, 0.0f};
    const auto pointDistance = [&](float t) { return (position + direction * t).length(); };
    float frontT = static_cast<float>(PROBE_MARCH_START);
    bool inFront = false;

    for (int leave = 0; leave < PROBE_LEAVE_STEPS && !inFront; ++leave) {
        inFront = parallaxMismatch(position, direction, frontT, filteredAt(parallaxDirection(position, direction, frontT))) < 0.0f;

        if (!inFront) {
            frontT *= 2.0f;
        }
    }

    if (!inFront) {
        return none;
    }

    const float end = far + position.length();
    const float texelAngle = probeTexelAngle(size);
    const std::uint32_t topLevel = levels - 1;
    std::uint32_t level = 0;

    for (int iteration = 0; iteration < PROBE_MARCH_ITERATIONS; ++iteration) {
        const Vec3 from = parallaxDirection(position, direction, frontT);

        // Once the probe's line of sight to the ray is within a texel of the ray's own (it turns towards it as the ray
        // goes on), the probe sees what the rest of the ray sees: read it along the ray. A ray past the far plane has
        // escaped the same way.
        if (std::acos(std::clamp(from.dot(direction), -1.0f, 1.0f)) <= texelAngle || frontT >= end) {
            return ParallaxLookup{direction, 1.0f};
        }

        const std::uint32_t side = std::max(size >> level, 1u);
        const float exitT = cubeCellExit(position, direction, frontT, cubeCellOf(from, side), side, end);
        const float nearest = cellAt(from, level);
        const float farthest = std::max(pointDistance(frontT), pointDistance(exitT));

        if (farthest < nearest) {
            frontT = exitT;
            level = std::min(level + 1, topLevel);
            continue;
        }

        if (level > 0) {
            --level;
            continue;
        }

        const Vec3 to = parallaxDirection(position, direction, exitT);
        const float stored = filteredAt(to);

        if (parallaxMismatch(position, direction, exitT, stored) < 0.0f) {
            // Far behind the texel's own surface yet in front where it leaves: it passed behind something thin the probe
            // saw in front of it (a wall seen edge-on), which it cannot judge.
            if (probeEdgeConfidence(1.0f - nearest / farthest) == 0.0f && probeEdgeConfidence(spreadAt(from)) < 1.0f) {
                return none;
            }

            frontT = exitT;
            continue;
        }

        float behindT = exitT;
        float behindDistance = stored;
        float frontDistance = filteredAt(from);

        for (int halving = 0; halving < PROBE_REFINE_STEPS; ++halving) {
            const float middle = 0.5f * (frontT + behindT);
            const float middleDistance = filteredAt(parallaxDirection(position, direction, middle));

            if (parallaxMismatch(position, direction, middle, middleDistance) < 0.0f) {
                frontT = middle;
                frontDistance = middleDistance;
            } else {
                behindT = middle;
                behindDistance = middleDistance;
            }
        }

        // Trusted where the probe's distances agree across the crossing and the texels blended there are one surface, or
        // where the probe looks at the point along the ray's own line, so it sees what the ray sees.
        const Vec3 found = parallaxDirection(position, direction, behindT);
        const float oneSurface = probeEdgeConfidence(spreadAt(found));
        const float alongTheRay = probeViewConfidence(std::acos(std::clamp(found.dot(direction), -1.0f, 1.0f)), size);
        return ParallaxLookup{found, std::max(parallaxConfidence(frontDistance, behindDistance) * oneSurface, alongTheRay)};
    }

    return none;
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
