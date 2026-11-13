#include "render/gpu/ambient_occlusion.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/screen_reflections.h"
#include "render/gpu/taa.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

void visit(AmbientOcclusionSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Ambient occlusion", settings.enabled);
    visitor.field("Occlusion radius", settings.radius, {.min = 0.01f, .max = 100.0f, .sliderMin = 0.05f, .sliderMax = 3.0f, .unit = "m"});
    visitor.field("Occlusion falloff", settings.falloff, {.min = 0.01f, .max = 1.0f});
    visitor.field("Occlusion slices", settings.slices, {.min = 1, .max = AO_MAX_SLICES});
    visitor.field("Occlusion steps", settings.steps, {.min = 1, .max = AO_MAX_STEPS});
    visitor.field("Occlusion thickness", settings.thickness, {.min = 0.001f, .max = 100.0f, .sliderMin = 0.01f, .sliderMax = 2.0f, .unit = "m"});
    visitor.field("Bounce light", settings.indirect);
    visitor.field("Bounce light radius", settings.indirectRadius, {.min = 0.01f, .max = 100.0f, .sliderMin = 0.2f, .sliderMax = 8.0f, .unit = "m"});
    visitor.field("Bounce light brightest sample", settings.indirectBrightest, {.min = 0.01f, .max = 1000.0f, .sliderMin = 1.0f, .sliderMax = 32.0f});
    visitor.field("Occlusion bounces", settings.multipleBounces);
    visitor.field("Specular occlusion", settings.specularOcclusion);
    visitor.field("Occlusion history", settings.temporal);
    visitor.field("Occlusion history frames", settings.historyFrames, {.min = 1, .max = AO_MAX_HISTORY_FRAMES});
    visitor.field("Occlusion history depth share", settings.historyDepthShare, {.min = 0.001f, .max = 1.0f, .sliderMin = 0.005f, .sliderMax = 0.2f});
    visitor.field("Occlusion anti-lag", settings.antiLag);
    visitor.field("Occlusion anti-lag deviations", settings.antiLagDeviations, {.min = 0.1f, .max = 20.0f, .sliderMin = 1.0f, .sliderMax = 6.0f});
    visitor.field("Occlusion history clip", settings.historyClip, {.min = 0.0f, .max = 20.0f, .sliderMin = 0.0f, .sliderMax = 6.0f});
    visitor.field("Occlusion denoise passes", settings.denoisePasses, {.min = 0, .max = AO_DENOISE_MAX_PASSES});
    visitor.field("Occlusion denoise plane share", settings.denoisePlaneShare, {.min = 0.001f, .max = 1.0f, .sliderMin = 0.002f, .sliderMax = 0.1f});
    visitor.field("Occlusion at half resolution", settings.halfResolution);
}

// Written so not a number fails too.
void validateAmbientOcclusion(const AmbientOcclusionSettings& settings) {
    if (!(settings.radius > 0.0f)) {
        throw std::invalid_argument("Ambient occlusion needs a radius above 0");
    }

    if (!(settings.indirectRadius > 0.0f)) {
        throw std::invalid_argument("Ambient occlusion needs an indirect radius above 0");
    }

    if (!(settings.indirectBrightest > 0.0f)) {
        throw std::invalid_argument("Ambient occlusion needs a brightest sample above 0");
    }

    if (!(settings.thickness > 0.0f)) {
        throw std::invalid_argument("Ambient occlusion needs a thickness above 0");
    }

    if (!(settings.falloff > 0.0f && settings.falloff <= 1.0f)) {
        throw std::invalid_argument("Ambient occlusion needs a falloff above 0 and at most 1");
    }

    if (settings.slices < 1 || settings.slices > AO_MAX_SLICES || settings.steps < 1 || settings.steps > AO_MAX_STEPS) {
        throw std::invalid_argument("Ambient occlusion needs 1 to AO_MAX_SLICES slices and 1 to AO_MAX_STEPS steps");
    }

    if (settings.historyFrames < 1 || settings.historyFrames > AO_MAX_HISTORY_FRAMES) {
        throw std::invalid_argument("Ambient occlusion needs 1 to AO_MAX_HISTORY_FRAMES history frames");
    }

    if (settings.denoisePasses < 0 || settings.denoisePasses > AO_DENOISE_MAX_PASSES) {
        throw std::invalid_argument("Ambient occlusion needs 0 to AO_DENOISE_MAX_PASSES denoise passes");
    }

    if (!(settings.antiLagDeviations > 0.0f)) {
        throw std::invalid_argument("Ambient occlusion needs anti-lag deviations above 0");
    }

    if (!(settings.historyClip >= 0.0f)) {
        throw std::invalid_argument("Ambient occlusion needs a history clip of at least 0");
    }

    if (!(settings.denoisePlaneShare > 0.0f)) {
        throw std::invalid_argument("Ambient occlusion needs a denoise plane share above 0");
    }

    if (!(settings.historyDepthShare > 0.0f)) {
        throw std::invalid_argument("Ambient occlusion needs a history depth share above 0");
    }
}

float occluderWeight(float distance, float radius, float falloff) {
    const float range = falloff * radius;
    return std::clamp((radius - distance) / range, 0.0f, 1.0f);
}

/*
* The slice's plane holds the way to the camera v and the direction d; its axis is at right angles to both. The normal
* projected into the plane has an angle from v, signed towards d's side (the part of d at right angles to v).
*/
AoSlice aoSlice(const Vec3& normal, const Vec3& toCamera, const Vec3& direction) {
    const Vec3 across = direction - toCamera * direction.dot(toCamera);
    const Vec3 axis = across.cross(toCamera).normalized();
    const Vec3 projected = normal - axis * normal.dot(axis);
    const float length = projected.length();

    if (!(length > 0.0f)) {
        return AoSlice{axis, 0.0f, 0.0f};
    }

    const float cosine = std::clamp(projected.dot(toCamera) / length, 0.0f, 1.0f);
    const float sign = across.dot(projected) < 0.0f ? -1.0f : 1.0f;
    return AoSlice{axis, sign * std::acos(cosine), length};
}

float lowHorizonCosine(const AoSlice& slice, int side) {
    const float halfTurn = 0.5f * std::numbers::pi_v<float>;
    return std::cos(side == 0 ? slice.normalAngle + halfTurn : slice.normalAngle - halfTurn);
}

float horizonCosine(const Vec3& offset, const Vec3& toCamera) {
    return offset.dot(toCamera) / offset.length();
}

/*
* Side 0's horizon is at +acos(cosine0) from the way to the camera, side 1's at -acos(cosine1); each is kept within a
* right angle of the normal (a horizon below the surface hides nothing). Each side's arc from the way to the camera
* integrates cos(theta - n) |sin theta| to (cos n + 2 h sin n - cos(2 h - n)) / 4.
*/
float sliceVisibility(const AoSlice& slice, float horizonCosine0, float horizonCosine1) {
    const float halfTurn = 0.5f * std::numbers::pi_v<float>;
    const float n = slice.normalAngle;
    const float h0 = n + std::max(-std::acos(std::clamp(horizonCosine1, -1.0f, 1.0f)) - n, -halfTurn);
    const float h1 = n + std::min(std::acos(std::clamp(horizonCosine0, -1.0f, 1.0f)) - n, halfTurn);
    const float cosine = std::cos(n);
    const float sine = std::sin(n);
    const float arc0 = (cosine + 2.0f * h0 * sine - std::cos(2.0f * h0 - n)) * 0.25f;
    const float arc1 = (cosine + 2.0f * h1 * sine - std::cos(2.0f * h1 - n)) * 0.25f;
    return slice.projectedLength * (arc0 + arc1);
}

Vec3 gatheredSampleLight(const Vec3& light, float brightest) {
    const float largest = std::max({light.x, light.y, light.z});
    return largest > brightest ? light * (brightest / largest) : light;
}

namespace {
    // The weight of the slice from the way to the camera out to angle h, either side (sliceVisibility's arc).
    float sliceArc(float normalAngle, float h) {
        return (std::cos(normalAngle) + 2.0f * h * std::sin(normalAngle) - std::cos(2.0f * h - normalAngle)) * 0.25f;
    }
}

/*
* The arc from the way to the camera is sliceArc on both sides, so the weight running from the low horizon up to an
* angle is the low side's arc less what is left between the angle and the way to the camera (or plus, past it). That over
* the slice's whole weight is a share with the same weight in every equal step, since the integrand is the light's own.
*/
float sectorShare(const AoSlice& slice, float directionCosine, int side) {
    const float halfTurn = 0.5f * std::numbers::pi_v<float>;
    const float n = slice.normalAngle;
    const float low = n - halfTurn;
    const float high = n + halfTurn;
    const float angle = std::acos(std::clamp(directionCosine, -1.0f, 1.0f));
    const float signedAngle = std::clamp(side == 0 ? angle : -angle, low, high);
    const float arcLow = sliceArc(n, low);
    const float arcHigh = sliceArc(n, high);
    const float arc = sliceArc(n, signedAngle);
    return std::clamp(((signedAngle < 0.0f ? -arc : arc) + arcLow) / (arcLow + arcHigh), 0.0f, 1.0f);
}

// Both ends rounded at the same offset; the bits between them are the arc's sectors (a surface's own samples, an arc of
// nothing, mark nothing).
std::uint32_t sectorMask(float share0, float share1, float offset) {
    const int sectors = AO_SECTORS;
    const int first = static_cast<int>(std::floor(std::min(share0, share1) * sectors + offset));
    const int last = static_cast<int>(std::floor(std::max(share0, share1) * sectors + offset));
    const int count = last - first;

    if (count <= 0) {
        return 0u;
    }

    // Shifting a 32-bit value by 32 is undefined, here and in GLSL.
    if (count >= sectors) {
        return 0xFFFFFFFFu;
    }

    return ((1u << count) - 1u) << first;
}

// Each side's miss: how far the pixel's point is from where its two neighbours on that side, carried on, put it.
Vec3 surfaceStep(const Vec3& centre, const Vec3& before, const Vec3& beforeNext, const Vec3& after, const Vec3& afterNext) {
    const Vec3 fromBefore = centre - before;
    const Vec3 toAfter = after - centre;

    if (!(fromBefore.dot(fromBefore) > 0.0f)) {
        return toAfter;
    }

    if (!(toAfter.dot(toAfter) > 0.0f)) {
        return fromBefore;
    }

    const Vec3 missBefore = centre - (before * 2.0f - beforeNext);
    const Vec3 missAfter = centre - (after * 2.0f - afterNext);
    return missAfter.dot(missAfter) < missBefore.dot(missBefore) ? toAfter : fromBefore;
}

Vec3 depthNormal(const Vec3& centre, const Vec3& across, const Vec3& downwards) {
    const Vec3 normal = downwards.cross(across);
    const float length = normal.length();

    if (!(length > 0.0f)) {
        return (centre * -1.0f).normalized();
    }

    const Vec3 unit = normal * (1.0f / length);
    return unit.dot(centre) > 0.0f ? unit * -1.0f : unit;
}

float aoScreenRadius(float radius, float footprint, float viewDistance) {
    return std::min(radius / (footprint * viewDistance), static_cast<float>(AO_MAX_SCREEN_RADIUS));
}

// Each channel moved on by its step a frame and wrapped (a Cranley-Patterson rotation of the tile).
Vec3 aoNoise(const Vec3& tileNoise, int frame) {
    const auto moved = [frame](float start, double step) {
        const float value = start + static_cast<float>(step) * static_cast<float>(frame);
        return value - std::floor(value);
    };
    return Vec3{moved(tileNoise.x, AO_SLICE_NOISE_FRAME_STEP), moved(tileNoise.y, AO_STEP_NOISE_FRAME_STEP), moved(tileNoise.z, AO_SECTOR_NOISE_FRAME_STEP)};
}

float aoSampleDistance(int step, int count, float noise, float screenRadius) {
    const float share = (static_cast<float>(step) + noise) / static_cast<float>(count);
    return std::max(share * share * screenRadius, static_cast<float>(AO_MIN_SAMPLE_PIXELS));
}

// The distance is ((step + noise) / count)^2 radius: its slope in the step, the spacing round the sample.
float aoSampleLevel(int step, int count, float noise, float screenRadius) {
    const float spacing = 2.0f * (static_cast<float>(step) + noise) * screenRadius / static_cast<float>(count * count);
    return std::log2(std::max(spacing, 1.0f));
}

// Jimenez et al. 2016, "Practical Real-Time Strategies for Accurate Indirect Occlusion", their equation 10.
Vec3 multiBounceOcclusion(float visibility, const Vec3& albedo) {
    const auto channel = [visibility](float albedo) {
        const float a = static_cast<float>(AO_BOUNCE_A_SLOPE) * albedo + static_cast<float>(AO_BOUNCE_A_OFFSET);
        const float b = static_cast<float>(AO_BOUNCE_B_SLOPE) * albedo + static_cast<float>(AO_BOUNCE_B_OFFSET);
        const float c = static_cast<float>(AO_BOUNCE_C_SLOPE) * albedo + static_cast<float>(AO_BOUNCE_C_OFFSET);
        return std::max(visibility, ((visibility * a + b) * visibility + c) * visibility);
    };
    return Vec3{channel(albedo.x), channel(albedo.y), channel(albedo.z)};
}

// Lagarde and de Rousiers 2014, "Moving Frostbite to Physically Based Rendering", their listing 26.
float specularOcclusion(float nDotV, float visibility, float roughness) {
    return std::clamp(std::pow(nDotV + visibility, std::exp2(-16.0f * roughness - 1.0f)) - 1.0f + visibility, 0.0f, 1.0f);
}

float surfaceVisibility(float screenVisibility, float occlusionTexel, float occlusionStrength) {
    return std::min(screenVisibility, 1.0f + occlusionStrength * (occlusionTexel - 1.0f));
}

float atrousKernelWeight(int dx, int dy) {
    constexpr float spline[3] = {6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f};
    return spline[std::abs(dx)] * spline[std::abs(dy)];
}

float aoPlaneWeight(const Vec3& centreNormal, const Vec3& centrePoint, const Vec3& tapPoint, float centreDepth, float share) {
    return std::exp(-std::abs(centreNormal.dot(tapPoint - centrePoint)) / (share * centreDepth));
}

float aoNormalWeight(const Vec3& centreNormal, const Vec3& tapNormal) {
    return std::pow(std::max(centreNormal.dot(tapNormal), 0.0f), static_cast<float>(AO_DENOISE_NORMAL_POWER));
}

// Any point along the ray over its view depth: perspective scales the whole point with its depth.
Vec3 aoDepthRay(const Mat4& inverseViewProjection, Vec2 pixel, Vec2 size) {
    return positionAtPixel(inverseViewProjection, pixel, 0.5f, size) * (1.0f / viewDistanceAtPixel(inverseViewProjection, pixel, 0.5f, size));
}

/*
* The taps' weights are normalised by their sum, which the centre's own (36/256, on its plane, its own normal) keeps above
* zero. The sky and points left open keep their value, and no tap reads them.
*/
Vec4 denoiseAoAt(int x, int y, int pass, const AoFrame& frame, const AmbientOcclusionSettings& settings, const AoGeometryImage& geometry,
                 const AoLightImage& light) {
    const Vec4 centreLight = light(x, y);
    const AoGeometry centre = geometry(x, y);

    if (centre.viewDepth <= 0.0f) {
        return centreLight;
    }

    const int width = static_cast<int>(frame.size.width);
    const int height = static_cast<int>(frame.size.height);
    const Vec2 size{static_cast<float>(width), static_cast<float>(height)};
    const Vec3 centrePoint = aoDepthRay(frame.inverseViewProjection, Vec2{x + 0.5f, y + 0.5f}, size) * centre.viewDepth;
    const int stride = 1 << pass;
    Vec4 sum;
    float weights = 0.0f;

    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int tx = x + dx * stride;
            const int ty = y + dy * stride;

            if (tx < 0 || ty < 0 || tx >= width || ty >= height) {
                continue;
            }

            const AoGeometry tap = geometry(tx, ty);

            if (tap.viewDepth <= 0.0f) {
                continue;
            }

            const Vec3 tapPoint = aoDepthRay(frame.inverseViewProjection, Vec2{tx + 0.5f, ty + 0.5f}, size) * tap.viewDepth;
            const float weight = atrousKernelWeight(dx, dy) * aoPlaneWeight(centre.normal, centrePoint, tapPoint, centre.viewDepth, settings.denoisePlaneShare) *
                                 aoNormalWeight(centre.normal, tap.normal);
            sum = sum + light(tx, ty) * weight;
            weights += weight;
        }
    }

    const Vec4 filtered = sum * (1.0f / weights);
    return Vec4{filtered.x, filtered.y, filtered.z, pass < AO_DENOISE_VISIBILITY_PASSES ? filtered.w : centreLight.w};
}
AoLight ambientOcclusionAt(int x, int y, const AoFrame& frame, const AmbientOcclusionSettings& settings, float sliceNoise, float stepNoise,
                           const AoDepthImage& depthAt, const AoPicture& picture, float sectorNoise) {
    const int width = static_cast<int>(frame.size.width);
    const int height = static_cast<int>(frame.size.height);
    const Vec2 size{static_cast<float>(width), static_cast<float>(height)};
    const float depth = depthAt(x, y);

    if (depth == gpuFarDepth) {
        return AoLight{};
    }

    const Vec2 pixel{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
    const Vec3 position = positionAtPixel(frame.inverseViewProjection, pixel, depth, size);
    const float screenRadius = aoScreenRadius(settings.radius, frame.footprint, viewDistanceAtPixel(frame.inverseViewProjection, pixel, depth, size));

    if (screenRadius < static_cast<float>(AO_MIN_SAMPLE_PIXELS)) {
        return AoLight{};
    }

    // The neighbours' points, clamped onto the screen (a neighbour off it lands on a texel already read, and its side's
    // step gives way: the nearest one off screen is the pixel itself); the sky, at no finite point, counts as the pixel.
    const auto neighbour = [&](int dx, int dy) {
        const int nx = std::clamp(x + dx, 0, width - 1);
        const int ny = std::clamp(y + dy, 0, height - 1);
        const float neighbourDepth = depthAt(nx, ny);
        return neighbourDepth == gpuFarDepth ? position : positionAtPixel(frame.inverseViewProjection, Vec2{nx + 0.5f, ny + 0.5f}, neighbourDepth, size);
    };
    const Vec3 across = surfaceStep(position, neighbour(-1, 0), neighbour(-2, 0), neighbour(1, 0), neighbour(2, 0));
    const Vec3 downwards = surfaceStep(position, neighbour(0, -1), neighbour(0, -2), neighbour(0, 1), neighbour(0, 2));
    const Vec3 normal = depthNormal(position, across, downwards);
    const Vec3 toCamera = (position * -1.0f).normalized();

    float visibility = 0.0f;
    Vec3 gathered;

    for (int slice = 0; slice < settings.slices; ++slice) {
        const float angle = std::numbers::pi_v<float> * (static_cast<float>(slice) + sliceNoise) / static_cast<float>(settings.slices);
        const Vec2 onScreen{std::cos(angle), std::sin(angle)};
        const Vec3 direction = positionAtPixel(frame.inverseViewProjection, pixel + onScreen, depth, size) - position;
        const AoSlice plane = aoSlice(normal, toCamera, direction);
        const float lows[2] = {lowHorizonCosine(plane, 0), lowHorizonCosine(plane, 1)};
        std::uint32_t marked = 0u;
        Vec3 sliceLight;

        // Each side remembers its last sample while the surface runs on, to fill the gap to the next.
        bool runsOn[2] = {false, false};
        float lastShare[2] = {0.0f, 0.0f};
        float lastDepth[2] = {0.0f, 0.0f};

        for (int step = 0; step < settings.steps; ++step) {
            const float distance = aoSampleDistance(step, settings.steps, stepNoise, screenRadius);

            for (int side = 0; side < 2; ++side) {
                const Vec2 sample = pixel + onScreen * (side == 0 ? distance : -distance);
                const int sx = static_cast<int>(std::floor(sample.x));
                const int sy = static_cast<int>(std::floor(sample.y));

                if (sx < 0 || sy < 0 || sx >= width || sy >= height) {
                    runsOn[side] = false;
                    continue;
                }

                const float sampleDepth = depthAt(sx, sy);

                if (sampleDepth == gpuFarDepth) {
                    runsOn[side] = false;
                    continue;
                }

                const Vec3 offset = positionAtPixel(frame.inverseViewProjection, Vec2{sx + 0.5f, sy + 0.5f}, sampleDepth, size) - position;
                const float weight = occluderWeight(offset.length(), settings.radius, settings.falloff);

                if (weight <= 0.0f) {
                    runsOn[side] = false;
                    continue;
                }

                const float low = lows[side];
                const float front = sectorShare(plane, low + (horizonCosine(offset, toCamera) - low) * weight, side);
                const float back = sectorShare(plane, low + (horizonCosine(offset - toCamera * settings.thickness, toCamera) - low) * weight, side);
                const float behind = -offset.dot(toCamera);
                float start = std::min(front, back);
                float end = std::max(front, back);

                if (runsOn[side] && std::abs(behind - lastDepth[side]) < settings.thickness) {
                    start = std::min(start, lastShare[side]);
                    end = std::max(end, lastShare[side]);
                }

                runsOn[side] = true;
                lastShare[side] = front;
                lastDepth[side] = behind;
                const std::uint32_t mask = sectorMask(start, end, sectorNoise);
                const std::uint32_t fresh = mask & ~marked;

                if (picture && fresh != 0u) {
                    const Vec3 seen = gatheredSampleLight(picture(sx, sy, aoSampleLevel(step, settings.steps, stepNoise, screenRadius) + frame.pictureLevelBias), settings.indirectBrightest);
                    sliceLight = sliceLight + seen * (static_cast<float>(std::popcount(fresh)) / static_cast<float>(AO_SECTORS));
                }

                marked |= mask;
            }
        }

        const float sliceWeight = sliceVisibility(plane, lows[0], lows[1]);
        visibility += sliceWeight * (1.0f - static_cast<float>(std::popcount(marked)) / static_cast<float>(AO_SECTORS));
        gathered = gathered + sliceLight * sliceWeight;
    }

    const float slices = static_cast<float>(settings.slices);
    return AoLight{gathered * (1.0f / slices), std::clamp(visibility / slices, 0.0f, 1.0f)};
}

float aoHistoryAge(float previousAge, int historyFrames) {
    return std::min(previousAge + 1.0f, static_cast<float>(historyFrames));
}

float aoCurrentShare(float age) {
    return 1.0f / std::max(age, 1.0f);
}

bool aoHistoryTapKept(float expectedDepth, float storedDepth, float share) {
    return storedDepth > 0.0f && std::abs(storedDepth - expectedDepth) <= share * expectedDepth;
}

/*
* The pixel's NDC and depth are a point of this frame's clip space with w = 1, that is the clip point over its w, 1 over
* the view depth now; the reprojection takes it to last frame's clip point over the same w, so its w is the view depth
* then over the view depth now. The pixel moves by the change in NDC (cameraMotion's form), so a still camera, whose
* reprojection is exactly the identity, gives exactly the same pixel.
*/
AoPreviousPlace aoPreviousPlace(const Mat4& reprojection, Vec2 pixel, float depth, float viewDepth, Vec2 size) {
    const Vec2 ndc = pixelToNdc(pixel, size);
    const Vec4 clip = reprojection * Vec4{ndc.x, ndc.y, depth, 1.0f};

    if (!(clip.w > 0.0f)) {
        return AoPreviousPlace{};
    }

    const Vec2 moved{clip.x / clip.w - ndc.x, clip.y / clip.w - ndc.y};
    const Vec2 previous{pixel.x + moved.x * 0.5f * size.x, pixel.y - moved.y * 0.5f * size.y};

    if (previous.x < 0.0f || previous.y < 0.0f || previous.x > size.x || previous.y > size.y) {
        return AoPreviousPlace{};
    }

    return AoPreviousPlace{previous, clip.w * viewDepth, true};
}

float aoTapWeight(Vec2 fraction, int tap) {
    return ((tap & 1) == 1 ? fraction.x : 1.0f - fraction.x) * ((tap >> 1) == 1 ? fraction.y : 1.0f - fraction.y);
}

// The four texels round the place, by their bilinear weights (texel centres at half pixels).
AoHistory readAoHistory(const AoHistoryImage& history, RenderSize size, const AoPreviousPlace& place, float share) {
    if (!place.valid) {
        return AoHistory{};
    }

    const float baseX = std::floor(place.pixel.x - 0.5f);
    const float baseY = std::floor(place.pixel.y - 0.5f);
    const Vec2 fraction{place.pixel.x - 0.5f - baseX, place.pixel.y - 0.5f - baseY};
    Vec4 light;
    float fastMean = 0.0f;
    float secondMoment = 0.0f;
    float weights = 0.0f;
    float age = 0.0f;

    for (int tap = 0; tap < 4; ++tap) {
        const int x = static_cast<int>(baseX) + (tap & 1);
        const int y = static_cast<int>(baseY) + (tap >> 1);
        const float weight = aoTapWeight(fraction, tap);

        if (weight <= 0.0f || x < 0 || y < 0 || x >= static_cast<int>(size.width) || y >= static_cast<int>(size.height)) {
            continue;
        }

        const AoHistoryTexel texel = history(x, y);

        if (!aoHistoryTapKept(place.viewDepth, texel.viewDepth, share)) {
            continue;
        }

        light = light + texel.light * weight;
        fastMean += texel.fastMean * weight;
        secondMoment += texel.secondMoment * weight;
        weights += weight;
        age += texel.age * weight;
    }

    if (!(weights > 0.0f)) {
        return AoHistory{};
    }

    return AoHistory{light * (1.0f / weights), age, fastMean / weights, secondMoment / weights};
}

Vec4 accumulatedAo(const Vec4& current, const Vec4& historyLight, float age) {
    return historyLight + (current - historyLight) * aoCurrentShare(age);
}

float aoLuminance(const Vec3& light) {
    return 0.2126f * light.x + 0.7152f * light.y + 0.0722f * light.z;
}

bool aoLightChanged(float fastMean, float slowMean, float secondMoment, float deviations) {
    const float share = static_cast<float>(AO_ANTILAG_FAST_SHARE);
    const float variance = std::max(secondMoment - slowMean * slowMean, 0.0f);
    const float fastSpread = std::sqrt(variance * share / (2.0f - share));
    const float floor = static_cast<float>(AO_ANTILAG_RELATIVE_FLOOR) * slowMean + static_cast<float>(AO_ANTILAG_ABSOLUTE_FLOOR);
    return std::abs(fastMean - slowMean) > std::max(deviations * fastSpread, floor);
}

AoClipBox aoNeighbourhood(int x, int y, RenderSize size, const AoDepthImage& depthAt, const AoLightImage& currentAt) {
    Vec4 sum;
    Vec4 squares;
    int count = 0;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int tapX = x + dx;
            const int tapY = y + dy;

            if (tapX < 0 || tapY < 0 || tapX >= static_cast<int>(size.width) || tapY >= static_cast<int>(size.height) || depthAt(tapX, tapY) == gpuFarDepth) {
                continue;
            }

            const Vec4 value = currentAt(tapX, tapY);
            sum = sum + value;
            squares = squares + Vec4{value.x * value.x, value.y * value.y, value.z * value.z, value.w * value.w};
            ++count;
        }
    }

    if (count == 0) {
        return AoClipBox{};
    }

    const float share = 1.0f / static_cast<float>(count);
    return AoClipBox{sum * share, squares * share, true};
}

namespace {
    // One channel: the same sums the shader does with vec4s.
    float clipAoChannel(float history, float mean, float meanOfSquares, float deviations) {
        const float spread = std::sqrt(std::max(meanOfSquares - mean * mean, 0.0f));
        return std::clamp(history, mean - deviations * spread, mean + deviations * spread);
    }
}

Vec4 clipAoHistory(const Vec4& historyLight, const Vec4& mean, const Vec4& meanOfSquares, float deviations) {
    if (!(deviations > 0.0f)) {
        return historyLight;
    }

    return Vec4{clipAoChannel(historyLight.x, mean.x, meanOfSquares.x, deviations), clipAoChannel(historyLight.y, mean.y, meanOfSquares.y, deviations),
                clipAoChannel(historyLight.z, mean.z, meanOfSquares.z, deviations), clipAoChannel(historyLight.w, mean.w, meanOfSquares.w, deviations)};
}

AoHistoryTexel accumulateAo(const Vec4& current, float viewDepth, const AoHistory& history, const AmbientOcclusionSettings& settings, const AoClipBox& box) {
    const float luminance = aoLuminance(Vec3{current.x, current.y, current.z});

    // Nothing to carry on: the frame alone, its noise estimate starting from it.
    if (!(history.age > 0.0f)) {
        return AoHistoryTexel{current, viewDepth, 1.0f, luminance, luminance * luminance};
    }

    const float fastMean = history.fastMean + (luminance - history.fastMean) * static_cast<float>(AO_ANTILAG_FAST_SHARE);
    const float slowMean = aoLuminance(Vec3{history.light.x, history.light.y, history.light.z});

    if (settings.antiLag && history.age >= static_cast<float>(AO_ANTILAG_LEAST_AGE) &&
        aoLightChanged(fastMean, slowMean, history.secondMoment, settings.antiLagDeviations)) {
        return AoHistoryTexel{current, viewDepth, 1.0f, luminance, luminance * luminance};
    }

    const float age = aoHistoryAge(history.age, settings.historyFrames);
    const float share = aoCurrentShare(age);
    const Vec4 historyLight = box.valid ? clipAoHistory(history.light, box.mean, box.meanOfSquares, settings.historyClip) : history.light;
    return AoHistoryTexel{accumulatedAo(current, historyLight, age), viewDepth, age, fastMean,
                          history.secondMoment + (luminance * luminance - history.secondMoment) * share};
}

AoHistoryTexel aoTemporalAt(int x, int y, const AoFrame& frame, const Mat4& reprojection, const AmbientOcclusionSettings& settings,
                            const AoDepthImage& depthAt, const Vec4& current, const AoHistoryImage& history, bool historyUsable,
                            const AoLightImage& currentAt) {
    const float depth = depthAt(x, y);

    if (depth == gpuFarDepth) {
        return AoHistoryTexel{Vec4{0.0f, 0.0f, 0.0f, 1.0f}, 0.0f, 0.0f, 0.0f, 0.0f};
    }

    const Vec2 size{static_cast<float>(frame.size.width), static_cast<float>(frame.size.height)};
    const Vec2 pixel{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
    const float viewDepth = viewDistanceAtPixel(frame.inverseViewProjection, pixel, depth, size);
    const AoHistory previous = historyUsable ? readAoHistory(history, frame.size, aoPreviousPlace(reprojection, pixel, depth, viewDepth, size), settings.historyDepthShare)
                                             : AoHistory{};
    const AoClipBox box = currentAt && settings.historyClip > 0.0f ? aoNeighbourhood(x, y, frame.size, depthAt, currentAt) : AoClipBox{};
    return accumulateAo(current, viewDepth, previous, settings, box);
}

bool aoHistoryUsable(const std::optional<AoHistoryKey>& last, const AoHistoryKey& now, bool previousCameraKnown) {
    return previousCameraKnown && last && last->size == now.size && last->gathers == now.gathers;
}

RenderSize aoHalfSize(RenderSize render) {
    return RenderSize{(render.width + 1) / 2, (render.height + 1) / 2};
}

// Corners first, then the other two, so two frames in a row already cover the block's diagonal.
AoTexel aoHalfPick(std::uint32_t frame) {
    constexpr AoTexel picks[4]{{0, 0}, {1, 1}, {1, 0}, {0, 1}};
    return picks[frame % 4];
}

Vec2 aoHalfShift(AoTexel pick) {
    return Vec2{static_cast<float>(pick.x) * 0.5f - 0.25f, static_cast<float>(pick.y) * 0.5f - 0.25f};
}

AoTexel aoHalfSourceTexel(int x, int y, AoTexel pick, RenderSize render) {
    return AoTexel{std::min(2 * x + pick.x, static_cast<int>(render.width) - 1), std::min(2 * y + pick.y, static_cast<int>(render.height) - 1)};
}

/*
* positionAtPixel reads (ndc, depth, 1) through the inverse, so moving every pixel by the shift is a translation of NDC by
* 2 shift / size (y flipped, as pixelToNdc counts it downwards) applied before it. Exact for an even render size; at an odd
* one the half-size grid is a little wider than the render, so a texel's point is off by up to a render pixel at the far edge.
*/
Mat4 aoShiftedInverseViewProjection(const Mat4& inverseViewProjection, Vec2 shift, RenderSize half) {
    return inverseViewProjection * Mat4::translation(2.0f * shift.x / static_cast<float>(half.width), -2.0f * shift.y / static_cast<float>(half.height), 0.0f);
}

// The shifted inverse reads half-grid coordinate u at the render's NDC of u + shift on the half grid, so u = pixel x
// half / render - shift.
Vec2 aoHalfPlace(Vec2 pixel, Vec2 renderSize, Vec2 halfSize, Vec2 shift) {
    return Vec2{pixel.x * halfSize.x / renderSize.x - shift.x, pixel.y * halfSize.y / renderSize.y - shift.y};
}

Vec4 aoUpsampleAt(int x, int y, const AoFrame& half, Vec2 shift, RenderSize render, float planeShare, const AoDepthImage& depthAt, const AoGeometryImage& geometry,
                  const AoLightImage& light) {
    const Vec4 open{0.0f, 0.0f, 0.0f, 1.0f};
    const float depth = depthAt(x, y);

    if (depth == gpuFarDepth) {
        return open;
    }

    const int halfWidth = static_cast<int>(half.size.width);
    const int halfHeight = static_cast<int>(half.size.height);
    const Vec2 halfSize{static_cast<float>(halfWidth), static_cast<float>(halfHeight)};
    const Vec2 place = aoHalfPlace(Vec2{x + 0.5f, y + 0.5f}, Vec2{static_cast<float>(render.width), static_cast<float>(render.height)}, halfSize, shift);
    const Vec3 point = positionAtPixel(half.inverseViewProjection, place, depth, halfSize);
    const float viewDepth = viewDistanceAtPixel(half.inverseViewProjection, place, depth, halfSize);
    const Vec2 base{std::floor(place.x - 0.5f), std::floor(place.y - 0.5f)};
    const Vec2 fraction{place.x - 0.5f - base.x, place.y - 0.5f - base.y};
    Vec4 sum;
    float weights = 0.0f;
    Vec4 nearest = open;
    float nearestGap = std::numeric_limits<float>::infinity();

    for (int tap = 0; tap < 4; ++tap) {
        const int tx = std::clamp(static_cast<int>(base.x) + (tap & 1), 0, halfWidth - 1);
        const int ty = std::clamp(static_cast<int>(base.y) + (tap >> 1), 0, halfHeight - 1);
        const AoGeometry texel = geometry(tx, ty);

        if (texel.viewDepth <= 0.0f) {
            continue;
        }

        const Vec4 texelLight = light(tx, ty);
        const float gap = std::abs(texel.viewDepth - viewDepth);

        if (gap < nearestGap) {
            nearestGap = gap;
            nearest = texelLight;
        }

        const Vec3 texelPoint = aoDepthRay(half.inverseViewProjection, Vec2{tx + 0.5f, ty + 0.5f}, halfSize) * texel.viewDepth;
        const float weight = aoTapWeight(fraction, tap) * aoPlaneWeight(texel.normal, texelPoint, point, texel.viewDepth, planeShare);
        sum = sum + texelLight * weight;
        weights += weight;
    }

    return weights >= static_cast<float>(AO_UPSAMPLE_LEAST_WEIGHT) ? sum * (1.0f / weights) : nearest;
}

GpuAmbientOcclusionPass packAmbientOcclusionPass(const Mat4& inverseViewProjection, const Mat4& viewProjection, const Mat4& reprojection, Vec2 jitter,
                                                 RenderSize render, const AmbientOcclusionSettings& settings, std::uint32_t frame, bool gathers,
                                                 bool accumulates) {
    GpuAmbientOcclusionPass pass{};
    const RenderSize size = settings.halfResolution ? aoHalfSize(render) : render;
    const AoTexel pick = settings.halfResolution ? aoHalfPick(frame) : AoTexel{};
    const Vec2 shift = settings.halfResolution ? aoHalfShift(pick) : Vec2{};
    writeColumnMajor(settings.halfResolution ? aoShiftedInverseViewProjection(inverseViewProjection, shift, size) : inverseViewProjection, pass.inverseViewProjection);
    const float width = static_cast<float>(size.width);
    const float height = static_cast<float>(size.height);
    pass.size[0] = width;
    pass.size[1] = height;
    pass.size[2] = 1.0f / width;
    pass.size[3] = 1.0f / height;
    pass.lens[0] = pixelFootprint(viewProjection, height);
    pass.lens[1] = jitter.x * width / static_cast<float>(render.width) - shift.x;
    pass.lens[2] = jitter.y * height / static_cast<float>(render.height) - shift.y;
    pass.lens[3] = std::log2(static_cast<float>(render.height) / height);
    pass.frame[1] = pick.x;
    pass.frame[2] = pick.y;
    pass.options[0] = gathers ? settings.indirectRadius : settings.radius;
    pass.options[1] = settings.falloff;
    pass.options[2] = static_cast<float>(settings.slices);
    pass.options[3] = static_cast<float>(settings.steps);
    pass.frame[0] = static_cast<std::int32_t>(frame);
    writeColumnMajor(reprojection, pass.reprojection);
    pass.gather[0] = settings.thickness;
    pass.gather[1] = gathers ? 1.0f : 0.0f;
    pass.gather[2] = settings.indirectBrightest;
    pass.history[0] = static_cast<float>(settings.historyFrames);
    pass.history[1] = settings.historyDepthShare;
    pass.history[2] = accumulates ? 1.0f : 0.0f;
    pass.denoise[0] = static_cast<float>(settings.denoisePasses);
    pass.denoise[1] = settings.denoisePlaneShare;
    pass.antiLag[0] = settings.antiLag ? 1.0f : 0.0f;
    pass.antiLag[1] = settings.antiLagDeviations;
    pass.antiLag[2] = settings.historyClip;
    return pass;
}
