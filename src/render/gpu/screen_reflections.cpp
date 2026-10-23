#include "render/gpu/screen_reflections.h"
#include "math/smooth_step.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/mip_levels.h"
#include "render/gpu/taa.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
    // A step that never comes: the ray does not move on screen along that axis.
    constexpr float never = 1e30f;
}

void visit(ReflectionSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Reflections", settings.enabled);
    visitor.field("Screen-space", settings.screenSpace);
    visitor.field("Probe", settings.probe);
    visitor.field("Sharp roughness", settings.sharpRoughness, {.min = 0.0f, .max = 1.0f});
    visitor.field("Max roughness", settings.maxRoughness, {.min = 0.0f, .max = 1.0f});
    visitor.field("Thickness", settings.thickness, {.min = 0.0f, .max = 10.0f, .sliderMax = 2.0f, .unit = "m"});
    visitor.field("Reach", settings.reach, {.min = 1.0f, .max = 1000.0f, .sliderMax = 300.0f, .unit = "m"});
    visitor.field("Max steps", settings.maxSteps, {.min = 1, .max = 512, .sliderMax = 256});
    visitor.field("Edge fade", settings.edgeFade, {.min = 0.01f, .max = 1.0f});
    visitor.field("Probe size", settings.probeSize, {.min = 16, .max = 1024, .sliderMin = 64, .sliderMax = 512, .unit = "texels"});
    visitor.field("Probe far", settings.probeFar, {.min = 1.0f, .max = 5000.0f, .sliderMax = 1000.0f, .unit = "m"});
    visitor.field("Show confidence", settings.showConfidence);
}

// The comparisons are written so not a number fails them too.
void validateReflections(const ReflectionSettings& settings) {
    if (!(settings.sharpRoughness >= 0.0f && settings.maxRoughness <= 1.0f && settings.sharpRoughness < settings.maxRoughness)) {
        throw std::invalid_argument("Screen-space reflections need 0 <= sharp roughness < max roughness <= 1");
    }

    if (!(settings.thickness >= 0.0f)) {
        throw std::invalid_argument("Screen-space reflections' thickness must not be negative");
    }

    if (!(settings.reach > 0.0f)) {
        throw std::invalid_argument("Screen-space reflections need a reach above 0");
    }

    if (settings.maxSteps < 1) {
        throw std::invalid_argument("Screen-space reflections need at least one step");
    }

    if (!(settings.edgeFade > 0.0f && settings.edgeFade <= 1.0f)) {
        throw std::invalid_argument("Screen-space reflections need an edge fade above 0 and at most 1");
    }

    if (settings.probeSize < 16 || settings.probeSize > 1024 || (settings.probeSize & (settings.probeSize - 1)) != 0) {
        throw std::invalid_argument("The reflection probe's size must be a power of two from 16 to 1024");
    }

    if (!(settings.probeFar > static_cast<float>(PROBE_NEAR))) {
        throw std::invalid_argument("The reflection probe must reach further than its near plane");
    }
}

std::uint32_t depthPyramidLevels(RenderSize size) {
    return std::min(mipLevelCount(size.width, size.height), static_cast<std::uint32_t>(SSR_MAX_PYRAMID_LEVELS));
}

RenderSize pyramidLevelSize(RenderSize size, std::uint32_t level) {
    return RenderSize{std::max(size.width >> level, std::uint32_t{1}), std::max(size.height >> level, std::uint32_t{1})};
}

/*
* Halving rounds down, so an odd level has one texel more than twice the level above: the last texel above takes it
* too (three texels in place of two), and every texel below has a cell above it, as the march needs.
*/
int pyramidLastFine(int coarse, int coarseSize, int fineSize) {
    return coarse == coarseSize - 1 ? fineSize - 1 : 2 * coarse + 1;
}

// The texels left over at the far edge belong to the last cell, which the clamp picks; a point a hair off the near
// edge belongs to the first.
int pyramidCell(int texel, std::uint32_t level, int levelSize) {
    return std::clamp(texel >> level, 0, levelSize - 1);
}

float DepthPyramid::nearest(std::uint32_t level, int x, int y) const {
    return levels[level][static_cast<std::size_t>(y) * sizes[level].width + static_cast<std::size_t>(x)];
}

// Reversed-Z: the nearer surface has the larger depth.
float nearerDepth(float a, float b) {
    return std::max(a, b);
}

DepthPyramid buildDepthPyramid(std::span<const float> depths, RenderSize size) {
    DepthPyramid pyramid;
    const std::uint32_t levelCount = depthPyramidLevels(size);
    pyramid.sizes.push_back(size);
    pyramid.levels.emplace_back(depths.begin(), depths.end());

    for (std::uint32_t level = 1; level < levelCount; ++level) {
        const RenderSize fine = pyramid.sizes.back();
        const RenderSize coarse = pyramidLevelSize(size, level);
        const std::vector<float>& below = pyramid.levels.back();
        std::vector<float> texels(static_cast<std::size_t>(coarse.width) * coarse.height);

        for (int y = 0; y < static_cast<int>(coarse.height); ++y) {
            for (int x = 0; x < static_cast<int>(coarse.width); ++x) {
                const int lastX = pyramidLastFine(x, static_cast<int>(coarse.width), static_cast<int>(fine.width));
                const int lastY = pyramidLastFine(y, static_cast<int>(coarse.height), static_cast<int>(fine.height));
                float nearest = gpuFarDepth;

                for (int fineY = 2 * y; fineY <= lastY; ++fineY) {
                    for (int fineX = 2 * x; fineX <= lastX; ++fineX) {
                        nearest = nearerDepth(nearest, below[static_cast<std::size_t>(fineY) * fine.width + static_cast<std::size_t>(fineX)]);
                    }
                }

                texels[static_cast<std::size_t>(y) * coarse.width + static_cast<std::size_t>(x)] = nearest;
            }
        }

        pyramid.sizes.push_back(coarse);
        pyramid.levels.push_back(std::move(texels));
    }

    return pyramid;
}

namespace {
    // The homogeneous point a pixel and depth come from: its position is xyz / w, and 1 / w is its clip w.
    Vec4 unprojectPixel(const Mat4& inverseViewProjection, Vec2 pixel, float depth, Vec2 size) {
        const Vec2 ndc = pixelToNdc(pixel, size);
        return inverseViewProjection * Vec4{ndc.x, ndc.y, depth, 1.0f};
    }

    // A clip position to its pixel and depth.
    Vec3 clipToScreen(const Vec4& clip, Vec2 size) {
        const Vec2 pixel = ndcToPixel(Vec2{clip.x / clip.w, clip.y / clip.w}, size);
        return Vec3{pixel.x, pixel.y, clip.z / clip.w};
    }

    // The largest t in 0..end for which start + step t stays within low..high along one axis (start is inside).
    float stayWithin(float start, float step, float low, float high, float end) {
        if (step > 0.0f) {
            return std::min(end, (high - start) / step);
        }

        if (step < 0.0f) {
            return std::min(end, (low - start) / step);
        }

        return end;
    }

    // How much t takes one axis out of its cell, the crossing past its edge; never, not moving along it.
    float axisExit(float position, float step, float cellSize) {
        if (step > 0.0f) {
            return ((std::floor(position / cellSize) + 1.0f) * cellSize + static_cast<float>(SSR_CELL_CROSSING) - position) / step;
        }

        if (step < 0.0f) {
            return (std::floor(position / cellSize) * cellSize - static_cast<float>(SSR_CELL_CROSSING) - position) / step;
        }

        return never;
    }

    HizTrace advance(HizTrace trace, const ScreenRay& ray, float t, int level) {
        trace.t += t;
        trace.position = ray.start + ray.step * trace.t;
        trace.level = level;
        return trace;
    }
}

Vec3 positionAtPixel(const Mat4& inverseViewProjection, Vec2 pixel, float depth, Vec2 size) {
    const Vec4 point = unprojectPixel(inverseViewProjection, pixel, depth, size);
    return Vec3{point.x / point.w, point.y / point.w, point.z / point.w};
}

float viewDistanceAtPixel(const Mat4& inverseViewProjection, Vec2 pixel, float depth, Vec2 size) {
    return 1.0f / unprojectPixel(inverseViewProjection, pixel, depth, size).w;
}

// The pixel's height on screen is 2 / height of NDC, and NDC y is the lens's scale times y / w.
float pixelFootprint(const Mat4& viewProjection, float height) {
    const Vec3 upRow{viewProjection.values[1][0], viewProjection.values[1][1], viewProjection.values[1][2]};
    return 2.0f / (upRow.length() * height);
}

/*
* Clip space is linear along the line in space, so the end point can be cut back to SSR_NEAREST_W in front of the camera
* by interpolating clip w. On screen, x / w, y / w and z / w are all linear in t between the two projected ends, so the
* screen's rectangle and the depth range cut it like a box.
*/
ScreenRay screenRay(const Mat4& viewProjection, const Vec3& position, const Vec3& direction, float reach, Vec2 size) {
    const Vec4 startClip = viewProjection * Vec4{position.x, position.y, position.z, 1.0f};
    Vec3 far = position + direction * reach;
    Vec4 endClip = viewProjection * Vec4{far.x, far.y, far.z, 1.0f};
    const float nearest = static_cast<float>(SSR_NEAREST_W);

    if (endClip.w < nearest && startClip.w > nearest) {
        far = position + direction * (reach * (startClip.w - nearest) / (startClip.w - endClip.w));
        endClip = viewProjection * Vec4{far.x, far.y, far.z, 1.0f};
    }

    ScreenRay ray;
    ray.start = clipToScreen(startClip, size);
    ray.step = clipToScreen(endClip, size) - ray.start;

    float end = 1.0f;
    end = stayWithin(ray.start.x, ray.step.x, 0.0f, size.x, end);
    end = stayWithin(ray.start.y, ray.step.y, 0.0f, size.y, end);
    end = stayWithin(ray.start.z, ray.step.z, gpuFarDepth, gpuNearDepth, end);
    ray.end = std::max(end, 0.0f);
    return ray;
}

float cellExit(Vec2 position, Vec2 step, float cellSize) {
    return std::min(axisExit(position.x, step.x, cellSize), axisExit(position.y, step.y, cellSize));
}

/*
* The cell's nearest depth is a plane nothing in the cell comes in front of. While the ray is in front of it, nothing
* in the cell can hide it up to where it reaches that depth, so it skips there (if that is still in the cell) or out of
* the cell; reaching the plane might be a hit, so it looks finer. Coming towards the camera it never reaches the plane.
*/
HizTrace hizStep(HizTrace trace, const ScreenRay& ray, float cellNearest, int topLevel) {
    if (!(trace.position.z > cellNearest)) {
        trace.level -= 1;
        return trace;
    }

    const float cellSize = static_cast<float>(1 << trace.level);
    const float toCellEdge = cellExit(Vec2{trace.position.x, trace.position.y}, Vec2{ray.step.x, ray.step.y}, cellSize);
    const float toSurface = ray.step.z < 0.0f ? (cellNearest - trace.position.z) / ray.step.z : never;

    if (toSurface < toCellEdge) {
        return advance(trace, ray, toSurface, trace.level - 1);
    }

    return advance(trace, ray, toCellEdge, std::min(trace.level + 1, topLevel));
}

HizTrace passTexel(HizTrace trace, const ScreenRay& ray) {
    return advance(trace, ray, cellExit(Vec2{trace.position.x, trace.position.y}, Vec2{ray.step.x, ray.step.y}, 1.0f), 0);
}

bool withinThickness(float rayDistance, float surfaceDistance, float thickness) {
    return rayDistance - surfaceDistance <= thickness;
}

/*
* The ray starts on its own surface's texel, so it leaves it first. A hit is the texel the ray reached or passed
* behind; one too far behind it is something the ray passes behind, and the march goes on beyond it.
*/
std::optional<ScreenHit> traceScreenRay(const DepthPyramid& pyramid, const ScreenRay& ray, const Mat4& inverseViewProjection,
                                        float thickness, int maxSteps) {
    const int topLevel = static_cast<int>(pyramid.levels.size()) - 1;
    const Vec2 size{static_cast<float>(pyramid.sizes[0].width), static_cast<float>(pyramid.sizes[0].height)};
    HizTrace trace = passTexel(HizTrace{ray.start, 0.0f, 0}, ray);

    for (int step = 0; step < maxSteps && trace.t <= ray.end; ++step) {
        const std::uint32_t level = static_cast<std::uint32_t>(trace.level);
        const RenderSize levelSize = pyramid.sizes[level];
        const int cellX = pyramidCell(static_cast<int>(std::floor(trace.position.x)), level, static_cast<int>(levelSize.width));
        const int cellY = pyramidCell(static_cast<int>(std::floor(trace.position.y)), level, static_cast<int>(levelSize.height));
        const float nearest = pyramid.nearest(level, cellX, cellY);
        const HizTrace next = hizStep(trace, ray, nearest, topLevel);

        if (next.level >= 0) {
            trace = next;
            continue;
        }

        if (next.t > ray.end) {
            break;
        }

        const Vec2 pixel{next.position.x, next.position.y};

        if (withinThickness(viewDistanceAtPixel(inverseViewProjection, pixel, next.position.z, size),
                            viewDistanceAtPixel(inverseViewProjection, pixel, nearest, size), thickness)) {
            return ScreenHit{next.position, next.t};
        }

        trace = passTexel(next, ray);
    }

    return std::nullopt;
}

float reflectionConfidence(const ReflectionHit& hit, Vec2 size, const ReflectionSettings& settings) {
    const float rough = 1.0f - smoothStep(settings.sharpRoughness, settings.maxRoughness, hit.roughness);

    // 0 at an edge, 1 at the centre: the nearest edge's distance as a share of the half-width (or half-height).
    const float u = hit.pixel.x / size.x;
    const float v = hit.pixel.y / size.y;
    const float inside = 2.0f * std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
    const float edge = smoothStep(0.0f, settings.edgeFade, inside);

    const float distance = 1.0f - smoothStep(static_cast<float>(SSR_DISTANCE_FADE_START) * settings.reach, settings.reach, hit.travelled);
    const float front = 1.0f - smoothStep(-static_cast<float>(SSR_FRONT_FACE_COSINE), 0.0f, hit.facing);
    return rough * edge * distance * front;
}

/*
* The cone's half angle has tangent SSR_CONE_SPREAD alpha, so after travelling d pixels it is 2 d tan wide; a level
* averages 2^level pixels, so the level as wide as the cone is log2 of its width (none narrower than a pixel).
*/
float reflectionConeLevel(float roughness, float travelledPixels) {
    const float width = 2.0f * travelledPixels * static_cast<float>(SSR_CONE_SPREAD) * roughness * roughness;
    return std::log2(std::max(width, 1.0f));
}

Vec3 reflectedScene(const Vec3& scene, const Vec3& standIn, const Vec3& specularAlbedo, const Vec3& hitLight, float confidence) {
    const Vec3 swapped = scene + (specularAlbedo * hitLight - standIn) * confidence;
    return Vec3{std::max(swapped.x, 0.0f), std::max(swapped.y, 0.0f), std::max(swapped.z, 0.0f)};
}

GpuReflectionPass packReflectionPass(const Mat4& viewProjection, const Mat4& inverseViewProjection, const Mat4& reprojection,
                                     RenderSize render, Vec2 jitter, const ReflectionSettings& settings, const ReflectionPassFrame& frame) {
    GpuReflectionPass pass{};
    writeColumnMajor(viewProjection, pass.viewProjection);
    writeColumnMajor(inverseViewProjection, pass.inverseViewProjection);
    writeColumnMajor(reprojection, pass.reprojection);

    const float width = static_cast<float>(render.width);
    const float height = static_cast<float>(render.height);
    pass.size[0] = width;
    pass.size[1] = height;
    pass.size[2] = 1.0f / width;
    pass.size[3] = 1.0f / height;

    pass.options[0] = settings.sharpRoughness;
    pass.options[1] = settings.maxRoughness;
    pass.options[2] = settings.thickness;
    pass.options[3] = settings.reach;

    pass.limits[0] = static_cast<float>(settings.maxSteps);
    pass.limits[1] = static_cast<float>(frame.pyramidLevels - 1);
    pass.limits[2] = settings.edgeFade;
    pass.limits[3] = settings.showConfidence ? 1.0f : 0.0f;

    pass.lens[0] = pixelFootprint(viewProjection, height);
    pass.lens[1] = jitter.x;
    pass.lens[2] = jitter.y;

    pass.probe[0] = frame.probeLevels > 0 ? 1.0f : 0.0f;
    pass.probe[1] = static_cast<float>(frame.probeLevels);
    pass.probe[2] = frame.screenSpace ? 1.0f : 0.0f;
    pass.probeOrigin[0] = frame.probeOrigin.x;
    pass.probeOrigin[1] = frame.probeOrigin.y;
    pass.probeOrigin[2] = frame.probeOrigin.z;
    return pass;
}

GpuPyramidPass packPyramidPass(RenderSize target, std::uint32_t level, RenderSize source) {
    GpuPyramidPass pass{};
    pass.target[0] = static_cast<std::int32_t>(target.width);
    pass.target[1] = static_cast<std::int32_t>(target.height);
    pass.target[2] = static_cast<std::int32_t>(level);
    pass.source[0] = static_cast<std::int32_t>(source.width);
    pass.source[1] = static_cast<std::int32_t>(source.height);
    return pass;
}
