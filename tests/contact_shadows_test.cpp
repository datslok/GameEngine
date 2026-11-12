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

    // On by default (off on Low), 16 steps over 0.3 m, a 0.2 m thickness, fading out from 40 to 60 m; reachable by key.
    void testTheSettings() {
        const ContactShadowSettings defaults;
        assert(defaults.enabled && defaults.length == 0.3f && defaults.steps == 16 && defaults.thickness == 0.2f);
        assert(defaults.fadeStart == 40.0f && defaults.fadeEnd == 60.0f);
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
        bad.thickness = 0.0f;
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
    * Blocked: the ray's point behind the surface the depth image shows there by more than the bias (so a surface never
    * shadows itself through the depth's rounding) and by less than the thickness (the depth image shows only fronts,
    * so a point far behind one passes behind the object). In front: never.
    */
    void testBlocked() {
        assert(contactBlocked(5.05f, 5.0f, 0.01f, 0.2f));
        assert(!contactBlocked(5.5f, 5.0f, 0.01f, 0.2f));
        assert(!contactBlocked(4.9f, 5.0f, 0.01f, 0.2f));
        assert(!contactBlocked(5.005f, 5.0f, 0.01f, 0.2f));
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

    std::vector<float> drawDepths(const StaticLevel& level, const Camera& camera, const Mat4& viewProjection, const Mat4& inverseViewProjection) {
        const Vec2 size{static_cast<float>(screen.width), static_cast<float>(screen.height)};
        std::vector<float> depths(static_cast<std::size_t>(screen.width) * screen.height, gpuFarDepth);

        for (std::uint32_t y = 0; y < screen.height; ++y) {
            for (std::uint32_t x = 0; x < screen.width; ++x) {
                const Vec3 direction = positionAtPixel(inverseViewProjection, Vec2{x + 0.5f, y + 0.5f}, 0.5f, size).normalized();
                const std::optional<LevelHit> hit = raycast(level, Ray{camera.getPosition(), direction}, 400.0f);

                if (hit) {
                    const Vec3 point = direction * hit->hit.distance;
                    const Vec4 clip = viewProjection * Vec4{point.x, point.y, point.z, 1.0f};
                    depths[static_cast<std::size_t>(y) * screen.width + x] = clip.z / clip.w;
                }
            }
        }

        return depths;
    }

    // The pixel a world point shows at.
    Vec2 pixelOf(const Mat4& viewProjection, const Camera& camera, const Vec3& world) {
        const Vec3 relative = world - camera.getPosition();
        const Vec3 screenPoint = clipToScreen(viewProjection * Vec4{relative.x, relative.y, relative.z, 1.0f},
                                              Vec2{static_cast<float>(screen.width), static_cast<float>(screen.height)});
        return Vec2{std::floor(screenPoint.x) + 0.5f, std::floor(screenPoint.y) + 0.5f};
    }

    /*
    * The march at real pixels, the sun low from +x: the floor just west of the block is shadowed by it (the shadow map
    * would miss so short a shadow at a grazing sun); the floor east of it, far from it, and the block's top are lit; the
    * sky is lit. With a thickness of a millimetre the march passes behind the block's front and finds nothing; with
    * the sun from the other side, the west floor is lit. Every step stays within the length.
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
        const Vec3 fromEast = Vec3{1.0f, 0.35f, 0.0f}.normalized();
        const ContactShadowView view = contactShadowView(viewProjection, inverseViewProjection, screen, fromEast, settings);

        const float shadowedWest = contactShadowAt(view, depthAt, pixelOf(viewProjection, camera, Vec3{-0.25f, 0.0f, 0.0f}), 0.5f);
        assert(shadowedWest < 0.01f);
        assert(contactShadowAt(view, depthAt, pixelOf(viewProjection, camera, Vec3{0.35f, 0.0f, 0.0f}), 0.5f) == 1.0f);
        assert(contactShadowAt(view, depthAt, pixelOf(viewProjection, camera, Vec3{-1.0f, 0.0f, 0.8f}), 0.5f) == 1.0f);
        assert(contactShadowAt(view, depthAt, pixelOf(viewProjection, camera, Vec3{0.0f, 0.3f, 0.0f}), 0.5f) == 1.0f);
        assert(contactShadowAt(view, depthAt, Vec2{160.5f, 0.5f}, 0.5f) == 1.0f);

        ContactShadowSettings thin = settings;
        thin.thickness = 0.001f;
        const ContactShadowView thinView = contactShadowView(viewProjection, inverseViewProjection, screen, fromEast, thin);
        assert(contactShadowAt(thinView, depthAt, pixelOf(viewProjection, camera, Vec3{-0.25f, 0.0f, 0.0f}), 0.5f) == 1.0f);

        const ContactShadowView fromWest = contactShadowView(viewProjection, inverseViewProjection, screen, Vec3{-1.0f, 0.35f, 0.0f}.normalized(), settings);
        assert(contactShadowAt(fromWest, depthAt, pixelOf(viewProjection, camera, Vec3{-0.25f, 0.0f, 0.0f}), 0.5f) == 1.0f);

        // Beyond the fade nothing is shadowed.
        ContactShadowSettings near = settings;
        near.fadeStart = 0.5f;
        near.fadeEnd = 1.0f;
        const ContactShadowView fadedView = contactShadowView(viewProjection, inverseViewProjection, screen, fromEast, near);
        assert(contactShadowAt(fadedView, depthAt, pixelOf(viewProjection, camera, Vec3{-0.25f, 0.0f, 0.0f}), 0.5f) == 1.0f);
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
        assert(pass.options[0] == 0.3f && pass.options[1] == 16.0f && pass.options[2] == 0.2f);
        assert(near(pass.options[3], contactNoiseTurn(3), 0.0f) && contactNoiseTurn(3) >= 0.0f && contactNoiseTurn(3) < 1.0f);
        assert(contactNoiseTurn(0) == 0.0f && contactNoiseTurn(1) != contactNoiseTurn(2));
        assert(pass.fade[0] == 40.0f && pass.fade[1] == 60.0f);
    }
}

void testContactShadows() {
    testTheSettings();
    testTheSteps();
    testBlocked();
    testTheFades();
    testTheMarch();
    testPacking();
}
