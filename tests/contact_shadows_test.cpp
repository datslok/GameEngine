#include "engine/settings_file.h"
#include "math/mat4.h"
#include "render/gpu/camera_relative.h"
#include "render/gpu/contact_shadows.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/quality_preset.h"
#include "render/gpu/render_settings.h"
#include "render/gpu/screen_reflections.h"
#include "scene/camera.h"

#include "test_levels.h"

#include <array>
#include <cassert>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    bool rejects(const ContactShadowSettings& settings) {
        try {
            validateContactShadows(settings);
        } catch (const std::invalid_argument&) {
            return true;
        }

        return false;
    }

    // On by default (off on Low), 32 steps over 0.3 m, fading out from 40 to 60 m; reachable by key. A step's reach, the
    // most a step may be behind a surface and still count, is the length over the steps.
    void testTheSettings() {
        const ContactShadowSettings defaults;
        assert(defaults.enabled && defaults.length == 0.3f && defaults.steps == 32);
        assert(defaults.fadeStart == 40.0f && defaults.fadeEnd == 60.0f);
        assert(defaults.localLights == CONTACT_MAX_LIGHTS && CONTACT_MAX_LIGHTS == 4);
        assert(contactStepReach(0.3f, 32) == 0.3f / 32.0f);
        validateContactShadows(defaults);

        ContactShadowSettings bad;
        bad.length = 0.0f;
        assert(rejects(bad));
        bad = {};
        bad.steps = 0;
        assert(rejects(bad));
        bad = {};
        bad.steps = CONTACT_MAX_STEPS + 1;
        assert(rejects(bad));
        bad = {};
        bad.localLights = -1;
        assert(rejects(bad));
        bad = {};
        bad.localLights = CONTACT_MAX_LIGHTS + 1;
        assert(rejects(bad));
        bad = {};
        bad.fadeEnd = bad.fadeStart - 1.0f;
        assert(rejects(bad));

        RenderSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };
        const std::vector<std::string> warnings =
            applySettings({{"contactShadows.enabled", "false"}, {"contactShadows.length", "0.5"}, {"contactShadows.steps", "8"}}, walk, StartupFields::Keep);
        assert(warnings.empty() && !settings.contactShadows.enabled && settings.contactShadows.length == 0.5f && settings.contactShadows.steps == 8);

        for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High, QualityPreset::Ultra}) {
            RenderSettings presetSettings;
            presetSettings.contactShadows.enabled = preset == QualityPreset::Low;
            applyQualityPreset(presetSettings, preset);
            assert(presetSettings.contactShadows.enabled == (preset != QualityPreset::Low));
            assert(matchingQualityPreset(presetSettings) == preset);
        }
    }

    // The steps: (step + noise) / steps of the length, in order, never past it; noise 0 starts at the surface.
    void testTheSteps() {
        float last = -1.0f;

        for (int step = 0; step < 16; ++step) {
            const float along = contactStepDistance(step, 16, 0.37f, 0.3f);
            assert(along > last && along <= 0.3f);
            assert(near(along, (static_cast<float>(step) + 0.37f) / 16.0f * 0.3f, 1e-7f));
            last = along;
        }

        assert(contactStepDistance(0, 16, 0.0f, 0.3f) == 0.0f);
        assert(contactStepDistance(15, 16, 0.999f, 0.3f) < 0.3f);
    }

    /*
    * Blocked: the step crossed a surface since the last one. It is now behind the surface the depth image shows there by
    * more than the bias (so a surface never shadows itself through the depth's rounding) and by less than the bias and a
    * step's reach (the depth image shows only fronts, and a ray that entered something within the last step can be no
    * further behind its front than that), and the last step was not (it was in front, within the bias, or passed over).
    */
    void testBlocked() {
        const float bias = 0.01f;
        const float reach = 0.02f;
        assert(contactBlocked(0.0f, 0.02f, bias, reach));
        assert(contactBlocked(-0.5f, 0.02f, bias, reach));
        assert(contactBlocked(bias, 0.029f, bias, reach));

        // In front, within the bias, or further behind than a step can reach (passing behind something nearer the camera).
        assert(!contactBlocked(0.0f, -0.1f, bias, reach));
        assert(!contactBlocked(0.0f, 0.005f, bias, reach));
        assert(!contactBlocked(0.0f, 0.031f, bias, reach));
        assert(!contactBlocked(0.0f, 0.5f, bias, reach));

        // Already behind a surface at the last step: no crossing.
        assert(!contactBlocked(0.015f, 0.02f, bias, reach));
        assert(!contactBlocked(0.5f, 0.02f, bias, reach));

        assert(near(contactBias(0.001f, 10.0f), static_cast<float>(CONTACT_BIAS_PIXELS) * 0.01f, 1e-7f));
    }

    // The fades: to nothing at the screen's edge over CONTACT_EDGE_PIXELS, and with view distance from start to end.
    void testTheFades() {
        const Vec2 size{320.0f, 180.0f};
        const float edge = static_cast<float>(CONTACT_EDGE_PIXELS);
        assert(contactEdgeFade(Vec2{0.0f, 90.0f}, size) == 0.0f);
        assert(contactEdgeFade(Vec2{160.0f, 90.0f}, size) == 1.0f);
        assert(near(contactEdgeFade(Vec2{edge * 0.5f, 90.0f}, size), 0.5f, 1e-6f));
        assert(near(contactEdgeFade(Vec2{160.0f, 180.0f - edge * 0.25f}, size), 0.25f, 1e-6f));
        assert(contactDistanceFade(10.0f, 40.0f, 60.0f) == 1.0f);
        assert(contactDistanceFade(70.0f, 40.0f, 60.0f) == 0.0f);
        assert(near(contactDistanceFade(50.0f, 40.0f, 60.0f), 0.5f, 1e-6f));
    }

    /*
    * A floor and a block 0.3 m a side at the origin, seen from 2.5 m off; the depths drawn by casting each pixel's ray
    * against the level (camera-relative, reversed-Z, as the prepass draws them).
    */
    constexpr RenderSize screen{320, 180};

    StaticLevel blockOnAFloor() {
        StaticLevel level = flatLevel();
        level.add(block(Vec3{-0.15f, 0.0f, -0.15f}, Vec3{0.15f, 0.3f, 0.15f}));
        return level;
    }

    Camera sceneCamera() {
        return Camera{Vec3{-1.8f, 1.2f, 1.6f}, Vec3{0.0f, 0.1f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f,
                      static_cast<float>(screen.width) / static_cast<float>(screen.height), 0.1f, 500.0f};
    }

    std::vector<float> drawDepths(const StaticLevel& level, const Camera& camera, const Mat4& viewProjection, const Mat4& inverseViewProjection,
                                  RenderSize image = screen) {
        const Vec2 size{static_cast<float>(image.width), static_cast<float>(image.height)};
        std::vector<float> depths(static_cast<std::size_t>(image.width) * image.height, gpuFarDepth);

        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const Vec3 direction = positionAtPixel(inverseViewProjection, Vec2{x + 0.5f, y + 0.5f}, 0.5f, size).normalized();
                const std::optional<LevelHit> hit = raycast(level, Ray{camera.getPosition(), direction}, 400.0f);

                if (hit) {
                    const Vec3 point = direction * hit->hit.distance;
                    const Vec4 clip = viewProjection * Vec4{point.x, point.y, point.z, 1.0f};
                    depths[static_cast<std::size_t>(y) * image.width + x] = clip.z / clip.w;
                }
            }
        }

        return depths;
    }

    // The pixel a world point shows at.
    Vec2 pixelOf(const Mat4& viewProjection, const Camera& camera, const Vec3& world, RenderSize image = screen) {
        const Vec3 relative = world - camera.getPosition();
        const Vec3 screenPoint = clipToScreen(viewProjection * Vec4{relative.x, relative.y, relative.z, 1.0f},
                                              Vec2{static_cast<float>(image.width), static_cast<float>(image.height)});
        return Vec2{std::floor(screenPoint.x) + 0.5f, std::floor(screenPoint.y) + 0.5f};
    }

    /*
    * The march at real pixels, the sun low from +x: the floor just west of the block is shadowed by it (the shadow map
    * would miss so short a shadow at a grazing sun); the floor east of it, far from it, and the block's top are lit; the
    * sky is lit. With the sun from the other side, the west floor is lit. Every step stays within the length.
    */
    void testTheMarch() {
        const StaticLevel level = blockOnAFloor();
        const Camera camera = sceneCamera();
        const Mat4 viewProjection = cameraRelativeViewProjection(camera);
        const Mat4 inverseViewProjection = *inverse(viewProjection);
        const std::vector<float> depths = drawDepths(level, camera, viewProjection, inverseViewProjection);
        const AoDepthImage depthAt = [&](int x, int y) {
            return depths[static_cast<std::size_t>(y) * screen.width + static_cast<std::size_t>(x)];
        };

        ContactShadowSettings settings;
        settings.length = 0.5f;
        settings.steps = 32;
        const Vec3 fromEast = Vec3{1.0f, 0.35f, 0.0f}.normalized();
        const ContactShadowView view = contactShadowView(viewProjection, inverseViewProjection, screen, fromEast, settings);

        const float shadowedWest = contactShadowAt(view, depthAt, pixelOf(viewProjection, camera, Vec3{-0.25f, 0.0f, 0.0f}), 0.5f);
        assert(shadowedWest < 0.01f);
        assert(contactShadowAt(view, depthAt, pixelOf(viewProjection, camera, Vec3{0.35f, 0.0f, 0.0f}), 0.5f) == 1.0f);
        assert(contactShadowAt(view, depthAt, pixelOf(viewProjection, camera, Vec3{-1.0f, 0.0f, 0.8f}), 0.5f) == 1.0f);
        assert(contactShadowAt(view, depthAt, pixelOf(viewProjection, camera, Vec3{0.0f, 0.3f, 0.0f}), 0.5f) == 1.0f);
        assert(contactShadowAt(view, depthAt, Vec2{160.5f, 0.5f}, 0.5f) == 1.0f);

        const ContactShadowView fromWest = contactShadowView(viewProjection, inverseViewProjection, screen, Vec3{-1.0f, 0.35f, 0.0f}.normalized(), settings);
        assert(contactShadowAt(fromWest, depthAt, pixelOf(viewProjection, camera, Vec3{-0.25f, 0.0f, 0.0f}), 0.5f) == 1.0f);

        // A cut-out (a fence's wire, a leaf) casts none: its pixels marked not solid are passed over, as their shadow map
        // already draws their holes; marked solid, the block shadows again.
        const AoDepthImage blockIsCutOut = [&](int x, int y) {
            const Vec2 size{static_cast<float>(screen.width), static_cast<float>(screen.height)};
            const float texelDepth = depthAt(x, y);

            if (texelDepth == gpuFarDepth) {
                return 1.0f;
            }

            const Vec3 world = positionAtPixel(inverseViewProjection, Vec2{x + 0.5f, y + 0.5f}, texelDepth, size) + camera.getPosition();
            return world.y > 0.01f ? 0.0f : 1.0f;
        };
        const AoDepthImage allSolid = [](int, int) { return 1.0f; };
        const Vec2 westFloor = pixelOf(viewProjection, camera, Vec3{-0.25f, 0.0f, 0.0f});
        assert(contactShadowAt(view, depthAt, westFloor, 0.5f, blockIsCutOut) == 1.0f);
        assert(contactShadowAt(view, depthAt, westFloor, 0.5f, allSolid) < 0.01f);

        // Beyond the fade nothing is shadowed.
        ContactShadowSettings near = settings;
        near.fadeStart = 0.5f;
        near.fadeEnd = 1.0f;
        const ContactShadowView fadedView = contactShadowView(viewProjection, inverseViewProjection, screen, fromEast, near);
        assert(contactShadowAt(fadedView, depthAt, pixelOf(viewProjection, camera, Vec3{-0.25f, 0.0f, 0.0f}), 0.5f) == 1.0f);
    }

    /*
    * A 1 cm pen standing on a floor, the sun low from +x, seen from the south (+z) and above at 640 x 360, a pixel about
    * 2 mm across there. Its true shadow on the floor west of it is 1 cm wide (|z| <= 5 mm). Rays from floor points north
    * of it (z < 0) pass behind the pen as the camera sees it: the old fixed thickness (5 cm) counted those up to 5 cm
    * behind its front as blocked, a shadow 4 cm too wide. Now a step counts only within its own reach, so the shadow is
    * too wide by at most that reach and the bias, and rays south of it pass in front and are never blocked.
    */
    constexpr RenderSize fine{640, 360};
    constexpr float penHalfWidth = 0.005f;

    struct PenScene {
        Camera camera{Vec3{0.0f, 0.35f, 0.55f}, Vec3{-0.04f, 0.02f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f,
                      static_cast<float>(fine.width) / static_cast<float>(fine.height), 0.05f, 100.0f};
        Mat4 viewProjection = cameraRelativeViewProjection(camera);
        Mat4 inverseViewProjection = *inverse(viewProjection);
        std::vector<float> depths;

        explicit PenScene(const StaticLevel& level) : depths(drawDepths(level, camera, viewProjection, inverseViewProjection, fine)) {
        }

        AoDepthImage depthAt() const {
            return [this](int x, int y) {
                return depths[static_cast<std::size_t>(y) * fine.width + static_cast<std::size_t>(x)];
            };
        }

        // The mask at a floor point averaged over the noise's turns, as the anti-aliasing averages the frames.
        float meanShadow(const ContactShadowSettings& settings, const Vec3& toLight, const Vec3& floorPoint) const {
            const ContactShadowView view = contactShadowView(viewProjection, inverseViewProjection, fine, toLight, settings);
            const Vec2 pixel = pixelOf(viewProjection, camera, floorPoint, fine);
            float sum = 0.0f;
            constexpr int turns = 8;

            for (int turn = 0; turn < turns; ++turn) {
                sum += contactShadowAt(view, depthAt(), pixel, (static_cast<float>(turn) + 0.5f) / static_cast<float>(turns));
            }

            return sum / static_cast<float>(turns);
        }

        // The view distance a world point is at, and a pixel's width there.
        float pixelWidthAt(const Vec3& world) const {
            const Vec3 relative = world - camera.getPosition();
            const float distance = (viewProjection * Vec4{relative.x, relative.y, relative.z, 1.0f}).w;
            return pixelFootprint(viewProjection, static_cast<float>(fine.height)) * distance;
        }
    };

    StaticLevel penOnAFloor() {
        StaticLevel level = flatLevel();
        level.add(block(Vec3{-penHalfWidth, 0.0f, -penHalfWidth}, Vec3{penHalfWidth, 0.14f, penHalfWidth}));
        return level;
    }

    // The span of z (north to south) along x = -5 cm where the floor is darkened at all (mean below 0.9), 1 mm apart.
    struct Band {
        float north = 1.0f;
        float south = -1.0f;
    };

    Band shadowBand(const PenScene& scene, const ContactShadowSettings& settings, const Vec3& toLight) {
        Band band;

        for (int millimetre = -60; millimetre <= 30; ++millimetre) {
            const float z = static_cast<float>(millimetre) * 0.001f;

            if (scene.meanShadow(settings, toLight, Vec3{-0.05f, 0.0f, z}) < 0.9f) {
                band.north = std::min(band.north, z);
                band.south = std::max(band.south, z);
            }
        }

        return band;
    }

    void testAThinPen() {
        const PenScene scene{penOnAFloor()};
        const Vec3 fromEast = Vec3{1.0f, 0.35f, 0.0f}.normalized();
        const Vec3 floorPoint{-0.05f, 0.0f, 0.0f};
        const float pixel = scene.pixelWidthAt(floorPoint);
        assert(pixel < 0.003f);

        for (const int steps : {16, 32}) {
            ContactShadowSettings settings;
            settings.steps = steps;
            const float reach = contactStepReach(settings.length, steps);
            const float bias = static_cast<float>(CONTACT_BIAS_PIXELS) * pixel;

            // Its shadow is there: right behind the pen it is dark on average (at 16 steps of 1.9 cm a step can jump
            // a 1 cm pen, so only most turns find it; at 32 every turn does).
            const float behind = scene.meanShadow(settings, fromEast, floorPoint);
            assert(steps == 16 ? behind < 0.6f : behind < 0.05f);

            // A step counts once it is the bias behind the pen's front and until it is a step's reach further: the shadow
            // is moved away from the camera by the bias (the near side loses up to the bias, never gains) and reaches
            // past the far side by at most the reach and the bias less the pen's own width (and a couple of pixels'
            // rounding). The old 5 cm thickness reached about 4 cm past it.
            const Band band = shadowBand(scene, settings, fromEast);
            assert(band.north <= -penHalfWidth);
            assert(band.north > penHalfWidth - reach - bias - 2.0f * pixel);
            assert(band.north > -0.025f);
            assert(band.south >= penHalfWidth - bias - 2.0f * pixel);
            assert(band.south < penHalfWidth + pixel);
        }

        // 32 steps make it tighter than 16.
        ContactShadowSettings coarse;
        coarse.steps = 16;
        ContactShadowSettings fineSteps;
        fineSteps.steps = 32;
        assert(shadowBand(scene, fineSteps, fromEast).north > shadowBand(scene, coarse, fromEast).north);
    }

    /*
    * A bar hanging between the camera and the floor, well clear of it: floor pixels beside its silhouette march into its
    * pixels, a metre behind its front. That is no crossing within a step, so the floor keeps no halo round it; only its
    * real shadow (towards the sun's side, further away) is in the shadow map's charge.
    */
    void testNoHaloRoundThingsInFront() {
        StaticLevel level = flatLevel();
        level.add(block(Vec3{-0.2f, 0.2f, 0.25f}, Vec3{0.2f, 0.22f, 0.27f}));
        const PenScene scene{level};
        const Vec3 fromNorth = Vec3{0.0f, 0.6f, -1.0f}.normalized();
        ContactShadowSettings settings;

        for (int millimetre = -40; millimetre <= 40; millimetre += 2) {
            const Vec3 floorPoint{static_cast<float>(millimetre) * 0.001f, 0.0f, 0.05f};
            assert(scene.meanShadow(settings, fromNorth, floorPoint) == 1.0f);
        }
    }

    /*
    * The way and length of a march towards a point or spot light from a point (camera-relative): towards the light, as
    * long as the settings' length but stopping the clearance short of the light (its own bulb's glowing mesh must not
    * shadow it); none out of its range, outside a spot's cone, or nearer the light than the clearance.
    */
    void testTheRayTowardsALight() {
        const ContactLight lamp{Vec3{0.0f, 2.0f, 0.0f}, 5.0f, Vec3{0.0f, 0.0f, -1.0f}, static_cast<float>(CONTACT_NO_CONE), 0.3f};
        const std::optional<ContactRay> up = contactLightRay(lamp, Vec3{0.0f, 0.0f, 0.0f}, 0.3f);
        assert(up && near(up->toLight.y, 1.0f, 1e-6f) && near(up->length, 0.3f, 1e-6f));

        const std::optional<ContactRay> close = contactLightRay(lamp, Vec3{0.0f, 1.5f, 0.0f}, 0.3f);
        assert(close && near(close->length, 0.2f, 1e-6f));
        assert(!contactLightRay(lamp, Vec3{0.0f, 1.8f, 0.0f}, 0.3f));

        ContactLight short_ = lamp;
        short_.range = 1.0f;
        assert(!contactLightRay(short_, Vec3{0.0f, 0.0f, 0.0f}, 0.3f));

        ContactLight downwards = lamp;
        downwards.beam = Vec3{0.0f, -1.0f, 0.0f};
        downwards.cosOuter = std::cos(0.44f);
        assert(contactLightRay(downwards, Vec3{0.1f, 0.0f, 0.0f}, 0.3f));
        ContactLight sideways = downwards;
        sideways.beam = Vec3{1.0f, 0.0f, 0.0f};
        assert(!contactLightRay(sideways, Vec3{0.1f, 0.0f, 0.0f}, 0.3f));

        // The clearance: the light's own glowing size and CONTACT_LIGHT_CLEARANCE past it.
        assert(near(contactLightStop(0.05f), 0.05f + static_cast<float>(CONTACT_LIGHT_CLEARANCE), 1e-6f));
    }

    /*
    * The march towards a lamp at real pixels: a lamp low in the east shadows the floor just west of the block, as the
    * low sun did; a lamp high over the floor point (the block not in the way), one out of reach and a spot turned away
    * leave it lit. A block round the lamp itself (its bulb's glowing mesh) casts nothing on the floor beside it, the march
    * stopping short of the light; with no clearance the march would walk into it.
    */
    void testTheMarchTowardsALamp() {
        const StaticLevel level = blockOnAFloor();
        const Camera camera = sceneCamera();
        const Mat4 viewProjection = cameraRelativeViewProjection(camera);
        const Mat4 inverseViewProjection = *inverse(viewProjection);
        const std::vector<float> depths = drawDepths(level, camera, viewProjection, inverseViewProjection);
        const AoDepthImage depthAt = [&](int x, int y) {
            return depths[static_cast<std::size_t>(y) * screen.width + static_cast<std::size_t>(x)];
        };

        ContactShadowSettings settings;
        settings.length = 0.5f;
        const ContactShadowView view = contactShadowView(viewProjection, inverseViewProjection, screen, Vec3{0.0f, 1.0f, 0.0f}, settings);
        const auto lampAt = [&](const Vec3& world, float range) {
            return ContactLight{world - camera.getPosition(), range, Vec3{0.0f, 0.0f, -1.0f}, static_cast<float>(CONTACT_NO_CONE), contactLightStop(0.05f)};
        };
        const Vec2 westFloor = pixelOf(viewProjection, camera, Vec3{-0.25f, 0.0f, 0.0f});

        assert(contactShadowTowards(view, depthAt, westFloor, 0.5f, lampAt(Vec3{1.0f, 0.35f, 0.0f}, 5.0f)) < 0.01f);
        assert(contactShadowTowards(view, depthAt, westFloor, 0.5f, lampAt(Vec3{-0.25f, 3.0f, 0.0f}, 5.0f)) == 1.0f);
        assert(contactShadowTowards(view, depthAt, westFloor, 0.5f, lampAt(Vec3{1.0f, 0.35f, 0.0f}, 1.0f)) == 1.0f);

        ContactLight turnedAway = lampAt(Vec3{1.0f, 0.35f, 0.0f}, 5.0f);
        turnedAway.beam = Vec3{1.0f, 0.0f, 0.0f};
        turnedAway.cosOuter = std::cos(0.44f);
        assert(contactShadowTowards(view, depthAt, westFloor, 0.5f, turnedAway) == 1.0f);

        // A bulb's glowing mesh round a lamp low beside the floor point, its west face towards the camera: the march stops
        // the clearance short of the lamp and never meets it; with no clearance it walks into that face.
        StaticLevel bulbLevel = flatLevel();
        bulbLevel.add(block(Vec3{0.5f, 0.05f, -0.1f}, Vec3{0.7f, 0.25f, 0.1f}));
        const std::vector<float> bulbDepths = drawDepths(bulbLevel, camera, viewProjection, inverseViewProjection);
        const AoDepthImage bulbDepthAt = [&](int x, int y) {
            return bulbDepths[static_cast<std::size_t>(y) * screen.width + static_cast<std::size_t>(x)];
        };
        const Vec2 besideBulb = pixelOf(viewProjection, camera, Vec3{0.3f, 0.0f, 0.0f});
        ContactLight bulb = lampAt(Vec3{0.6f, 0.15f, 0.0f}, 5.0f);
        assert(contactShadowTowards(view, bulbDepthAt, besideBulb, 0.5f, bulb) == 1.0f);
        bulb.stop = 0.0f;
        assert(contactShadowTowards(view, bulbDepthAt, besideBulb, 0.5f, bulb) < 0.01f);
    }

    /*
    * The lights that get a contact shadow channel: only seated lights holding a shadow, ranked by how brightly they light
    * the focus (light priority's measure), at most count; last frame's keep theirs unless a rival is clearly brighter
    * (incumbentAdvantage). Each is named in the frame's data by its slot, spotlights past CONTACT_SPOT_SLOT_BASE, -1 for an
    * empty channel.
    */
    void testChoosingTheLights() {
        const auto point = [](float x, std::uint32_t entity, bool shadowed) {
            PlacedPointLight placed;
            placed.position = Vec3{x, 2.0f, 0.0f};
            placed.light.castsShadows = shadowed;
            placed.entity = Entity{entity, 1};
            return placed;
        };
        FrameLighting seated;
        seated.pointLights = {point(10.0f, 1, true), point(0.0f, 2, false), point(1.0f, 3, true), point(30.0f, 4, true)};
        PlacedSpotLight spot;
        spot.position = Vec3{0.5f, 2.0f, 0.0f};
        spot.entity = Entity{5, 1};
        seated.spotLights = {spot};
        const Vec3 focus{0.0f, 0.0f, 0.0f};

        const std::vector<ContactLightPick> two = chooseContactLights(seated, focus, {}, 2, 1.25f);
        assert(two.size() == 2);
        assert(two[0].kind == ContactLightKind::Spot && two[0].slot == 0);
        assert(two[1].kind == ContactLightKind::Point && two[1].slot == 2);

        const std::vector<ContactLightPick> all = chooseContactLights(seated, focus, {}, 4, 1.25f);
        assert(all.size() == 4);
        for (const ContactLightPick& pick : all) {
            assert(!(pick.kind == ContactLightKind::Point && pick.slot == 1));
        }
        assert(chooseContactLights(seated, focus, {}, 0, 1.25f).empty());

        // The point light at 1 m lights the focus a little less than the spot at 0.5 m: holding a channel, it keeps it.
        const std::vector<ContactLightPick> kept = chooseContactLights(seated, focus, {Entity{3, 1}}, 1, 1.25f);
        assert(kept.size() == 1 && kept[0].kind == ContactLightKind::Point && kept[0].slot == 2);

        const std::array<std::int32_t, CONTACT_MAX_LIGHTS> codes = contactLightSlotCodes(two);
        assert(codes[0] == CONTACT_SPOT_SLOT_BASE && codes[1] == 2 && codes[2] == -1 && codes[3] == -1);

        // A pick's march target, measured from the camera, with its range, cone and clearance.
        const Vec3 cameraPosition{0.0f, 1.0f, 5.0f};
        const ContactLight target = contactLightFor(seated, two[1], cameraPosition);
        assert(near(target.position.x, 1.0f, 1e-6f) && near(target.position.y, 1.0f, 1e-6f) && near(target.position.z, -5.0f, 1e-6f));
        assert(target.range == seated.pointLights[2].light.range && target.cosOuter == static_cast<float>(CONTACT_NO_CONE));
        assert(near(target.stop, contactLightStop(seated.pointLights[2].light.emitterRadius), 1e-6f));
        const ContactLight spotTarget = contactLightFor(seated, two[0], cameraPosition);
        assert(near(spotTarget.cosOuter, std::cos(spot.light.outerAngle), 1e-6f));
    }

    // The pass record: the matrices column-major, the size and its inverse, the way to the light with the pixel's
    // footprint, the options and the frame's noise turn, the fades.
    void testPacking() {
        const Camera camera = sceneCamera();
        const Mat4 viewProjection = cameraRelativeViewProjection(camera);
        const Mat4 inverseViewProjection = *inverse(viewProjection);
        const ContactShadowSettings settings;
        const Vec3 toLight = Vec3{1.0f, 0.35f, 0.0f}.normalized();
        const GpuContactShadowPass pass = packContactShadowPass(viewProjection, inverseViewProjection, screen, toLight, settings, 3);
        assert(pass.viewProjection[1] == viewProjection.values[1][0] && pass.inverseViewProjection[4] == inverseViewProjection.values[0][1]);
        assert(pass.size[0] == 320.0f && pass.size[1] == 180.0f && near(pass.size[2], 1.0f / 320.0f, 1e-9f));
        assert(pass.toLight[0] == toLight.x && near(pass.toLight[3], pixelFootprint(viewProjection, 180.0f), 1e-9f));
        assert(pass.options[0] == 0.3f && pass.options[1] == 32.0f && pass.options[2] == 0.3f / 32.0f);
        assert(near(pass.options[3], contactNoiseTurn(3), 0.0f) && contactNoiseTurn(3) >= 0.0f && contactNoiseTurn(3) < 1.0f);
        assert(contactNoiseTurn(0) == 0.0f && contactNoiseTurn(1) != contactNoiseTurn(2));
        assert(pass.fade[0] == 40.0f && pass.fade[1] == 60.0f);

        // Without lights: the sun alone. With them: each light's place and range, beam and cone, clearance, and the
        // count and whether the sun marches too.
        assert(pass.lightOptions[0] == 0.0f && pass.lightOptions[1] == 1.0f);
        const std::vector<ContactLight> lamps{ContactLight{Vec3{1.0f, 2.0f, 3.0f}, 7.0f, Vec3{0.0f, -1.0f, 0.0f}, 0.9f, 0.3f}};
        const GpuContactShadowPass withLamps = packContactShadowPass(viewProjection, inverseViewProjection, screen, toLight, settings, 3, lamps, false);
        assert(withLamps.lightOptions[0] == 1.0f && withLamps.lightOptions[1] == 0.0f);
        assert(withLamps.lightPositionRange[0][0] == 1.0f && withLamps.lightPositionRange[0][2] == 3.0f && withLamps.lightPositionRange[0][3] == 7.0f);
        assert(withLamps.lightBeam[0][1] == -1.0f && withLamps.lightBeam[0][3] == 0.9f && withLamps.lightStop[0] == 0.3f);
    }
}

void testContactShadows() {
    testTheSettings();
    testTheSteps();
    testBlocked();
    testTheFades();
    testTheMarch();
    testAThinPen();
    testNoHaloRoundThingsInFront();
    testTheRayTowardsALight();
    testTheMarchTowardsALamp();
    testChoosingTheLights();
    testPacking();
}
