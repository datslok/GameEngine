#include "render/gpu/contact_shadows.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/screen_reflections.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

void visit(ContactShadowSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Enabled", settings.enabled);
    visitor.field("Length", settings.length, {.min = 0.01f, .max = 10.0f, .sliderMin = 0.05f, .sliderMax = 1.0f, .unit = "m"});
    visitor.field("Steps", settings.steps, {.min = 1, .max = CONTACT_MAX_STEPS, .sliderMin = 4, .sliderMax = 32});
    visitor.field("Thickness", settings.thickness, {.min = 0.001f, .max = 10.0f, .sliderMin = 0.02f, .sliderMax = 1.0f, .unit = "m"});
    visitor.field("Fade start", settings.fadeStart, {.min = 0.0f, .max = 10000.0f, .sliderMin = 5.0f, .sliderMax = 200.0f, .unit = "m"});
    visitor.field("Fade end", settings.fadeEnd, {.min = 0.0f, .max = 10000.0f, .sliderMin = 5.0f, .sliderMax = 200.0f, .unit = "m"});
}

// Written so not a number fails too.
void validateContactShadows(const ContactShadowSettings& settings) {
    if (!(settings.length > 0.0f) || !(settings.thickness > 0.0f)) {
        throw std::invalid_argument("Contact shadows need a length and a thickness above 0");
    }

    if (settings.steps < 1 || settings.steps > CONTACT_MAX_STEPS) {
        throw std::invalid_argument("Contact shadows need 1 to CONTACT_MAX_STEPS steps");
    }

    if (!(settings.fadeStart >= 0.0f) || !(settings.fadeEnd >= settings.fadeStart)) {
        throw std::invalid_argument("Contact shadows' fade must start at 0 or more and end no sooner than it starts");
    }
}

float contactStepDistance(int step, int steps, float noise, float length) {
    return (static_cast<float>(step) + noise) / static_cast<float>(steps) * length;
}

float contactBias(float footprint, float viewDistance) {
    return static_cast<float>(CONTACT_BIAS_PIXELS) * footprint * viewDistance;
}

float contactEdgeFade(Vec2 pixel, Vec2 size) {
    const float nearest = std::min({pixel.x, pixel.y, size.x - pixel.x, size.y - pixel.y});
    return std::clamp(nearest / static_cast<float>(CONTACT_EDGE_PIXELS), 0.0f, 1.0f);
}

bool contactBlocked(float rayDistance, float sceneDistance, float bias, float thickness) {
    const float behind = rayDistance - sceneDistance;
    return behind > bias && behind < thickness;
}

// A fade of no width cuts off at its start.
float contactDistanceFade(float viewDistance, float fadeStart, float fadeEnd) {
    if (!(fadeEnd > fadeStart)) {
        return viewDistance <= fadeStart ? 1.0f : 0.0f;
    }

    return std::clamp((fadeEnd - viewDistance) / (fadeEnd - fadeStart), 0.0f, 1.0f);
}

// In double, as the frame count grows large, then wrapped.
float contactNoiseTurn(std::uint32_t frame) {
    const double turn = static_cast<double>(frame) * CONTACT_NOISE_FRAME_STEP;
    return static_cast<float>(turn - std::floor(turn));
}

ContactShadowView contactShadowView(const Mat4& viewProjection, const Mat4& inverseViewProjection, RenderSize size, const Vec3& toLight,
                                    const ContactShadowSettings& settings) {
    const float height = static_cast<float>(size.height);
    return ContactShadowView{viewProjection, inverseViewProjection, Vec2{static_cast<float>(size.width), height}, pixelFootprint(viewProjection, height),
                             toLight, settings};
}

/*
* Each step's point, measured from the camera, through the view-projection to its pixel and view distance (clip w);
* the depth image's surface there to its own view distance; the first blocked step ends the march.
*/
float contactShadowAt(const ContactShadowView& view, const AoDepthImage& depthAt, Vec2 pixel, float noise) {
    const int x = static_cast<int>(std::floor(pixel.x));
    const int y = static_cast<int>(std::floor(pixel.y));
    const float depth = depthAt(x, y);

    if (depth == gpuFarDepth) {
        return 1.0f;
    }

    const float distance = viewDistanceAtPixel(view.inverseViewProjection, pixel, depth, view.size);
    const float fade = contactDistanceFade(distance, view.settings.fadeStart, view.settings.fadeEnd);

    if (!(fade > 0.0f)) {
        return 1.0f;
    }

    const Vec3 start = positionAtPixel(view.inverseViewProjection, pixel, depth, view.size);
    const float bias = contactBias(view.footprint, distance);

    for (int step = 0; step < view.settings.steps; ++step) {
        const Vec3 point = start + view.toLight * contactStepDistance(step, view.settings.steps, noise, view.settings.length);
        const Vec4 clip = view.viewProjection * Vec4{point.x, point.y, point.z, 1.0f};

        if (clip.w < static_cast<float>(CONTACT_NEAREST_W)) {
            break;
        }

        const Vec3 onScreen = clipToScreen(clip, view.size);

        if (onScreen.x < 0.0f || onScreen.y < 0.0f || onScreen.x >= view.size.x || onScreen.y >= view.size.y) {
            break;
        }

        const int texelX = static_cast<int>(onScreen.x);
        const int texelY = static_cast<int>(onScreen.y);
        const float sceneDepth = depthAt(texelX, texelY);

        if (sceneDepth == gpuFarDepth) {
            continue;
        }

        const Vec2 texelCentre{static_cast<float>(texelX) + 0.5f, static_cast<float>(texelY) + 0.5f};
        const float sceneDistance = viewDistanceAtPixel(view.inverseViewProjection, texelCentre, sceneDepth, view.size);

        if (contactBlocked(clip.w, sceneDistance, bias, view.settings.thickness)) {
            return 1.0f - fade * contactEdgeFade(Vec2{onScreen.x, onScreen.y}, view.size);
        }
    }

    return 1.0f;
}

GpuContactShadowPass packContactShadowPass(const Mat4& viewProjection, const Mat4& inverseViewProjection, RenderSize size, const Vec3& toLight,
                                           const ContactShadowSettings& settings, std::uint32_t frame) {
    GpuContactShadowPass pass{};
    writeColumnMajor(viewProjection, pass.viewProjection);
    writeColumnMajor(inverseViewProjection, pass.inverseViewProjection);
    const float width = static_cast<float>(size.width);
    const float height = static_cast<float>(size.height);
    pass.size[0] = width;
    pass.size[1] = height;
    pass.size[2] = 1.0f / width;
    pass.size[3] = 1.0f / height;
    pass.toLight[0] = toLight.x;
    pass.toLight[1] = toLight.y;
    pass.toLight[2] = toLight.z;
    pass.toLight[3] = pixelFootprint(viewProjection, height);
    pass.options[0] = settings.length;
    pass.options[1] = static_cast<float>(settings.steps);
    pass.options[2] = settings.thickness;
    pass.options[3] = contactNoiseTurn(frame);
    pass.fade[0] = settings.fadeStart;
    pass.fade[1] = settings.fadeEnd;
    return pass;
}
