#include "render/gpu/contact_shadows.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/light_priority.h"
#include "render/gpu/screen_reflections.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

void visit(ContactShadowSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Enabled", settings.enabled);
    visitor.field("Length", settings.length, {.min = 0.01f, .max = 10.0f, .sliderMin = 0.05f, .sliderMax = 1.0f, .unit = "m"});
    visitor.field("Steps", settings.steps, {.min = 1, .max = CONTACT_MAX_STEPS, .sliderMin = 4, .sliderMax = CONTACT_MAX_STEPS});
    visitor.field("Fade start", settings.fadeStart, {.min = 0.0f, .max = 10000.0f, .sliderMin = 5.0f, .sliderMax = 200.0f, .unit = "m"});
    visitor.field("Fade end", settings.fadeEnd, {.min = 0.0f, .max = 10000.0f, .sliderMin = 5.0f, .sliderMax = 200.0f, .unit = "m"});
    visitor.field("Local lights", settings.localLights, {.min = 0, .max = CONTACT_MAX_LIGHTS});
}

// Written so not a number fails too.
void validateContactShadows(const ContactShadowSettings& settings) {
    if (!(settings.length > 0.0f)) {
        throw std::invalid_argument("Contact shadows need a length above 0");
    }

    if (settings.steps < 1 || settings.steps > CONTACT_MAX_STEPS) {
        throw std::invalid_argument("Contact shadows need 1 to CONTACT_MAX_STEPS steps");
    }

    if (settings.localLights < 0 || settings.localLights > CONTACT_MAX_LIGHTS) {
        throw std::invalid_argument("Contact shadows take 0 to CONTACT_MAX_LIGHTS local lights");
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

float contactStepReach(float length, int steps) {
    return length / static_cast<float>(steps);
}

/*
* The last step in front of its surface (or within the bias), this one just inside something: no further behind its front
* than a step goes. A last step behind something (inside it, or behind a thing nearer the camera) is no crossing.
*/
bool contactBlocked(float previousBehind, float behind, float bias, float reach) {
    return previousBehind <= bias && behind > bias && behind < bias + reach;
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

namespace {
    // A pixel's surface: where it is (camera-relative), its view distance, and the distance fade there; none for the sky
    // or past the fade.
    struct ContactStart {
        Vec3 position;
        float distance = 0.0f;
        float fade = 0.0f;
    };

    std::optional<ContactStart> contactStart(const ContactShadowView& view, const AoDepthImage& depthAt, Vec2 pixel) {
        const float depth = depthAt(static_cast<int>(std::floor(pixel.x)), static_cast<int>(std::floor(pixel.y)));

        if (depth == gpuFarDepth) {
            return std::nullopt;
        }

        const float distance = viewDistanceAtPixel(view.inverseViewProjection, pixel, depth, view.size);
        const float fade = contactDistanceFade(distance, view.settings.fadeStart, view.settings.fadeEnd);

        if (!(fade > 0.0f)) {
            return std::nullopt;
        }

        return ContactStart{positionAtPixel(view.inverseViewProjection, pixel, depth, view.size), distance, fade};
    }

    /*
    * Each step's point, measured from the camera, through the view-projection to its pixel and view distance (clip w);
    * the depth image's surface there to its own view distance; the first step that crossed a surface ends the march.
    * The start lies on its own surface, and a step passed over (the sky, a cut-out) counts as in front. A step's reach
    * is the march's own length over the steps, shorter for a march cut short by its light.
    */
    float contactMarch(const ContactShadowView& view, const AoDepthImage& depthAt, const ContactStart& start, const ContactRay& ray, float noise,
                       const AoDepthImage& solidAt) {
        const float bias = contactBias(view.footprint, start.distance);
        const float reach = contactStepReach(ray.length, view.settings.steps);
        const float fade = start.fade;
        float previousBehind = 0.0f;

        for (int step = 0; step < view.settings.steps; ++step) {
            const Vec3 point = start.position + ray.toLight * contactStepDistance(step, view.settings.steps, noise, ray.length);
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

            if (sceneDepth == gpuFarDepth || (solidAt && solidAt(texelX, texelY) < 0.5f)) {
                previousBehind = 0.0f;
                continue;
            }

            const Vec2 texelCentre{static_cast<float>(texelX) + 0.5f, static_cast<float>(texelY) + 0.5f};
            const float behind = clip.w - viewDistanceAtPixel(view.inverseViewProjection, texelCentre, sceneDepth, view.size);

            if (contactBlocked(previousBehind, behind, bias, reach)) {
                return 1.0f - fade * contactEdgeFade(Vec2{onScreen.x, onScreen.y}, view.size);
            }

            previousBehind = behind;
        }

        return 1.0f;
    }
}

float contactShadowAt(const ContactShadowView& view, const AoDepthImage& depthAt, Vec2 pixel, float noise, const AoDepthImage& solidAt) {
    const std::optional<ContactStart> start = contactStart(view, depthAt, pixel);
    return start ? contactMarch(view, depthAt, *start, ContactRay{view.toLight, view.settings.length}, noise, solidAt) : 1.0f;
}

float contactLightStop(float emitterRadius) {
    return std::max(emitterRadius, 0.0f) + static_cast<float>(CONTACT_LIGHT_CLEARANCE);
}

/*
* A spot's cone is tested on the way from the light to the point, as the main pass does; a point light's cosine is
* below any real one.
*/
std::optional<ContactRay> contactLightRay(const ContactLight& light, const Vec3& start, float maxLength) {
    const Vec3 offset = light.position - start;
    const float distance = offset.length();

    if (!(distance < light.range) || !(distance > light.stop)) {
        return std::nullopt;
    }

    const Vec3 toLight = offset * (1.0f / distance);

    if (!(toLight.dot(light.beam) * -1.0f > light.cosOuter)) {
        return std::nullopt;
    }

    return ContactRay{toLight, std::min(maxLength, distance - light.stop)};
}

float contactShadowTowards(const ContactShadowView& view, const AoDepthImage& depthAt, Vec2 pixel, float noise, const ContactLight& light,
                           const AoDepthImage& solidAt) {
    const std::optional<ContactStart> start = contactStart(view, depthAt, pixel);

    if (!start) {
        return 1.0f;
    }

    const std::optional<ContactRay> ray = contactLightRay(light, start->position, view.settings.length);
    return ray ? contactMarch(view, depthAt, *start, *ray, noise, solidAt) : 1.0f;
}

/*
* Every seated light holding a shadow (castsShadows is on only for those), ranked as light priority ranks seats; the
* sort is stable, so equally bright lights keep their order.
*/
std::vector<ContactLightPick> chooseContactLights(const FrameLighting& seated, const Vec3& focus, const std::vector<Entity>& previous, int count,
                                                  float incumbentAdvantage) {
    struct Candidate {
        ContactLightPick pick;
        float standing = 0.0f;
    };

    std::vector<Candidate> candidates;
    const auto consider = [&](ContactLightKind kind, std::size_t slot, const Vec3& position, const auto& light, Entity entity) {
        if (!light.castsShadows) {
            return;
        }

        const float importance = lightImportance(position, light.colour, light.intensity, light.sourceRadius, focus);
        const bool held = entity != Entity{} && std::find(previous.begin(), previous.end(), entity) != previous.end();
        candidates.push_back(Candidate{ContactLightPick{kind, static_cast<int>(slot), entity}, held ? importance * incumbentAdvantage : importance});
    };

    for (std::size_t slot = 0; slot < seated.pointLights.size(); ++slot) {
        const PlacedPointLight& placed = seated.pointLights[slot];
        consider(ContactLightKind::Point, slot, placed.position, placed.light, placed.entity);
    }

    for (std::size_t slot = 0; slot < seated.spotLights.size(); ++slot) {
        const PlacedSpotLight& placed = seated.spotLights[slot];
        consider(ContactLightKind::Spot, slot, placed.position, placed.light, placed.entity);
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.standing > b.standing; });
    std::vector<ContactLightPick> picks;

    for (std::size_t index = 0; index < candidates.size() && static_cast<int>(index) < count; ++index) {
        picks.push_back(candidates[index].pick);
    }

    return picks;
}

std::array<std::int32_t, CONTACT_MAX_LIGHTS> contactLightSlotCodes(const std::vector<ContactLightPick>& picks) {
    std::array<std::int32_t, CONTACT_MAX_LIGHTS> codes;
    codes.fill(-1);

    for (std::size_t channel = 0; channel < picks.size() && channel < codes.size(); ++channel) {
        const ContactLightPick& pick = picks[channel];
        codes[channel] = pick.kind == ContactLightKind::Spot ? CONTACT_SPOT_SLOT_BASE + pick.slot : pick.slot;
    }

    return codes;
}

ContactLight contactLightFor(const FrameLighting& seated, const ContactLightPick& pick, const Vec3& cameraPosition) {
    if (pick.kind == ContactLightKind::Spot) {
        const PlacedSpotLight& placed = seated.spotLights.at(static_cast<std::size_t>(pick.slot));
        return ContactLight{placed.position - cameraPosition, placed.light.range, placed.light.direction.normalized(), std::cos(placed.light.outerAngle),
                            contactLightStop(placed.light.emitterRadius)};
    }

    const PlacedPointLight& placed = seated.pointLights.at(static_cast<std::size_t>(pick.slot));
    return ContactLight{placed.position - cameraPosition, placed.light.range, Vec3{0.0f, 0.0f, -1.0f}, static_cast<float>(CONTACT_NO_CONE),
                        contactLightStop(placed.light.emitterRadius)};
}

GpuContactShadowPass packContactShadowPass(const Mat4& viewProjection, const Mat4& inverseViewProjection, RenderSize size, const Vec3& toLight,
                                           const ContactShadowSettings& settings, std::uint32_t frame, std::span<const ContactLight> lights, bool sun) {
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
    pass.options[2] = contactStepReach(settings.length, settings.steps);
    pass.options[3] = contactNoiseTurn(frame);
    pass.fade[0] = settings.fadeStart;
    pass.fade[1] = settings.fadeEnd;

    const std::size_t count = std::min(lights.size(), static_cast<std::size_t>(CONTACT_MAX_LIGHTS));

    for (std::size_t light = 0; light < count; ++light) {
        const ContactLight& target = lights[light];
        pass.lightPositionRange[light][0] = target.position.x;
        pass.lightPositionRange[light][1] = target.position.y;
        pass.lightPositionRange[light][2] = target.position.z;
        pass.lightPositionRange[light][3] = target.range;
        pass.lightBeam[light][0] = target.beam.x;
        pass.lightBeam[light][1] = target.beam.y;
        pass.lightBeam[light][2] = target.beam.z;
        pass.lightBeam[light][3] = target.cosOuter;
        pass.lightStop[light] = target.stop;
    }

    pass.lightOptions[0] = static_cast<float>(count);
    pass.lightOptions[1] = sun ? 1.0f : 0.0f;
    return pass;
}
