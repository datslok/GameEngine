#include "games/lightlab/light_lab_game.h"

#include "games/lightlab/light_lab_level.h"
#include "gameplay/first_person_body.h"
#include "math/angles.h"
#include "math/transform.h"
#include "scene/model_renderer.h"
#include "scene/sky_object.h"
#include "scene/sun.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
    // How far out the sun disc is drawn, as a share of the camera's far plane: beyond the ground's edge (400 m) and every
    // tower, but inside the far plane so it is not clipped away.
    constexpr float sunDiscDistanceShare = 0.9f;

    // The views' starts: the free camera behind the start looking north along the towers, the MOBA view just ahead of
    // the start, the body at the origin; the far plane past the furthest tower (320 m) and the ground's corners.
    const PlayerControlsSetup labViews{
        .freeCameraPosition = Vec3{0.0f, 2.0f, 6.0f},
        .freeCameraTarget = Vec3{0.0f, 2.5f, -30.0f},
        .mobaLookPoint = Vec3{0.0f, 0.0f, -8.0f},
        .firstPersonFeet = Vec3{0.0f, 0.0f, 0.0f},
        .verticalFovDegrees = 70.0f,
        .nearPlane = 0.1f,
        .farPlane = 1000.0f
    };

    // The sun fades in over the last moments of sunrise (its disc is about half a degree across; refraction and the
    // horizon's haze blur it more), and is full strength above that.
    constexpr float sunFadeBelowDegrees = -1.0f;
    constexpr float sunFullAboveDegrees = 4.0f;

    // Low sunlight crosses far more air, which scatters blue away (the same Rayleigh scattering that makes the sky
    // blue), so the sun reddens towards the horizon: a blend from the high sun's colour to the low one's below this height.
    constexpr float sunWarmBelowDegrees = 30.0f;
    const Vec3 highSunColour{1.0f, 0.96f, 0.9f};
    const Vec3 lowSunColour{1.0f, 0.55f, 0.3f};

    // The sky: blue, full from this height up, dimming through dusk (civil twilight ends with the sun
    // 6 degrees down) to a small night share, not black.
    const Vec3 skyTint{0.6f, 0.75f, 1.0f};
    constexpr float skyDarkBelowDegrees = -6.0f;
    constexpr float skyFullAboveDegrees = 20.0f;
    constexpr float nightSkyShare = 0.03f;

    /*
    * 0 below edge0, 1 above edge1, and the smooth 3t^2 - 2t^3 between, as GLSL's smoothstep. Kept here rather than
    * shared: the renderer's copy is part of its mirrored shader maths, which games may not include.
    */
    float smoothStep(float edge0, float edge1, float x) {
        const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // Surfaces in sRGB and how rough they are: matte grass, bark and leaves; plaster, painted wood and stone a little
    // smoother; varnished floorboards smoother still.
    Material surfaceMaterial(LabSurface surface) {
        Material material;
        switch (surface) {
        case LabSurface::Stone:
            material.colour = Pixel{150, 148, 140};
            material.roughness = 0.7f;
            break;
        case LabSurface::Wood:
            material.colour = Pixel{125, 95, 65};
            material.roughness = 0.8f;
            break;
        case LabSurface::Bark:
            material.colour = Pixel{90, 70, 50};
            material.roughness = 0.95f;
            break;
        case LabSurface::Leaves:
            material.colour = Pixel{60, 110, 50};
            material.roughness = 0.9f;
            break;
        case LabSurface::Floorboards:
            material.colour = Pixel{170, 130, 90};
            material.roughness = 0.6f;
            break;
        case LabSurface::Plaster:
            material.colour = Pixel{220, 215, 205};
            material.roughness = 0.9f;
            break;
        case LabSurface::RedPaint:
            material.colour = Pixel{185, 30, 25};
            material.roughness = 0.8f;
            break;
        }
        return material;
    }

    // One group of blocks as the parts of one renderer: the unit cube (-1..1) stretched to each half-size and moved.
    ModelRenderer blocksRenderer(MeshHandle cube, const std::vector<LabBlock>& blocks) {
        ModelRenderer renderer;
        for (const LabBlock& block : blocks) {
            const Mat4 placement = Mat4::translation(block.centre.x, block.centre.y, block.centre.z) *
                                   Mat4::scaling(block.halfSize.x, block.halfSize.y, block.halfSize.z);
            renderer.parts.push_back(RenderPart{cube, surfaceMaterial(block.surface), placement});
        }
        return renderer;
    }

    /*
    * The golden image views (engine/golden.h), each from the free camera at a fixed time of day: the towers and fence from
    * the start (sun shadows in four cascades out to 200 m), the field from above, the sunlit room's beam on its
    * floor (the rest of the room lit by the light probes: sunlight bounced off the floor, and the sky through the
    * window), the towers again at sunset in HDR (a low,
    * red sun and long shadows), and the sun itself, its disc and the glow bloom spreads round it.
    */
    struct LabGoldenView {
        GoldenView view;
        Vec3 cameraPosition;
        Vec3 cameraTarget;
        float solarHours;

        // Look straight at the sun instead of at the target (which then is ignored), wherever it stands at that hour.
        bool facingSun = false;
    };

    const LabGoldenView labGoldenViews[] = {
        {{"towers", 0.5, false}, Vec3{0.0f, 1.7f, 4.0f}, Vec3{0.0f, 3.0f, -40.0f}, 15.5f},
        {{"overview", 0.5, false}, Vec3{40.0f, 45.0f, 50.0f}, Vec3{-10.0f, 0.0f, -30.0f}, 15.5f},
        {{"sunlit-room", 0.5, false}, Vec3{-22.3f, 2.2f, 14.4f}, Vec3{-18.6f, 0.3f, 10.6f}, 15.5f},
        {{"sunset-hdr", 0.5, true}, Vec3{0.0f, 1.7f, 4.0f}, Vec3{0.0f, 3.0f, -40.0f}, 17.6f},
        {{"sun-disc", 0.5, false}, Vec3{0.0f, 1.7f, 4.0f}, Vec3{}, 15.5f, true}
    };
}

void visit(LightLabSunSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Latitude", settings.latitudeDegrees, {.min = -90.0f, .max = 90.0f, .unit = "deg"});
    visitor.field("Day of year", settings.dayOfYear, {.min = 1.0f, .max = daysPerYear});
    visitor.field("Solar hours", settings.solarHours, {.min = 0.0f, .max = hoursPerDay, .unit = "h"});
    visitor.field("Hours per second", settings.hoursPerSecond, {.min = 0.0f, .max = hoursPerDay, .sliderMax = 1.0f, .unit = "h/s"});
    visitor.field("Sun intensity", settings.sunIntensity, {.min = 0.0f, .sliderMax = 10.0f});
    visitor.field("Sky brightness", settings.skyBrightness, {.min = 0.0f, .sliderMax = 5.0f});
    visitor.field("Sun disc size", settings.sunDiscScale, {.min = 0.0f, .sliderMax = 10.0f, .unit = "x real"});
}

void visit(LightLabProbeSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Dense probes (20 cm, building only)", settings.denseProbes);
}

float advanceSolarHours(float solarHours, float hoursPerSecond, float seconds) {
    if (hoursPerSecond == 0.0f) {
        return solarHours;
    }

    const float advanced = std::fmod(solarHours + hoursPerSecond * seconds, hoursPerDay);
    return advanced < 0.0f ? advanced + hoursPerDay : advanced;
}

Daylight daylightFor(const Vec3& towardsSun, const LightLabSunSettings& settings) {
    const float elevation = sunElevation(towardsSun);
    const float sunUp = smoothStep(radiansFromDegrees(sunFadeBelowDegrees), radiansFromDegrees(sunFullAboveDegrees), elevation);
    const float warmth = 1.0f - smoothStep(0.0f, radiansFromDegrees(sunWarmBelowDegrees), elevation);
    const float skyUp = smoothStep(radiansFromDegrees(skyDarkBelowDegrees), radiansFromDegrees(skyFullAboveDegrees), elevation);

    Daylight daylight;
    daylight.sun.direction = towardsSun * -1.0f;
    daylight.sun.colour = highSunColour + (lowSunColour - highSunColour) * warmth;
    daylight.sun.intensity = settings.sunIntensity * sunUp;
    daylight.sun.castsShadows = true;
    const float skyShare = nightSkyShare + (1.0f - nightSkyShare) * skyUp;
    daylight.background.colour = skyTint * (settings.skyBrightness * skyShare);
    daylight.sky.colour = daylight.background.colour;
    return daylight;
}

LightLabGame::LightLabGame(ControlMode startMode, bool debugModeSwitching):
    controls(labViews, startMode, debugModeSwitching)
{
}

const char* LightLabGame::title() const {
    return "Light lab";
}

/*
* The ground and each group of blocks are one entity each, and all of it is static level: the triangles drawn are the
* triangles walked on, made from the same renderers.
*/
void LightLabGame::onInit(World& world, AssetManager& assets) {
    const MeshHandle cube = assets.addMesh(Mesh::cube());
    const LightLabLayout layout = buildLightLabLayout();

    Material grass;
    grass.colour = Pixel{75, 110, 75};
    grass.roughness = 1.0f;

    const auto addStatic = [&](ModelRenderer renderer) {
        staticLevel.add(assets.makeCollisionMesh(renderer));
        const Entity entity = world.create();
        world.add(entity, Transform{});
        world.add(entity, std::move(renderer));
    };

    addStatic(makeMeshRenderer(assets.addMesh(Mesh::plane(labGroundHalfSize)), grass));
    addStatic(blocksRenderer(cube, layout.towers));
    addStatic(blocksRenderer(cube, layout.fence));
    addStatic(blocksRenderer(cube, layout.trees));
    addStatic(blocksRenderer(cube, layout.building));

    controls.spawnBody(world);

    sun = world.create();
    world.add(sun, DirectionalLight{});
    sky = world.create();
    world.add(sky, AmbientLight{});
    world.add(sky, SkyBackground{});
    probes = world.create();
    world.add(probes, labProbeVolume);
    flashlight.create(world, false);

    // The sun disc: a glowing sphere with no colour of its own. It stands for the sunlight's source, so it must not
    // block that light. It teleports with the camera every frame, so it has no PreviousTransform to blend from.
    Material sunGlow;
    sunGlow.colour = Pixel{0, 0, 0};
    sunGlow.roughness = 1.0f;
    ModelRenderer discRenderer = makeMeshRenderer(assets.addMesh(Mesh::sphere()), sunGlow);
    discRenderer.castsShadows = false;
    sunDisc = world.create();
    world.add(sunDisc, Transform{});
    world.add(sunDisc, std::move(discRenderer));

    // Start from the mode's predictable camera pose, then light the day around it.
    controls.setControlMode(world, controls.getMode());
    updateDaylight(world);
}

void LightLabGame::onInput(World& world, const Input& input) {
    controls.updateFirstPersonCommands(world, input);

    if (input.wasKeyPressed(Key::F4)) {
        debugViewEnabled = !debugViewEnabled;
    }

    if (input.wasKeyPressed(Key::F)) {
        flashlight.toggle(world);
    }
}

/*
* The clock advances in ticks, so the time of day depends only on how long the game has run, never on the frame rate.
*/
void LightLabGame::onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) {
    (void)simulationSeconds;
    sunSettings.solarHours = advanceSolarHours(sunSettings.solarHours, sunSettings.hoursPerSecond, tickSeconds);
    updateFirstPersonBodies(world, staticLevel, tickSeconds);
}

// The daylight follows the settings every frame, so dragging a slider moves the sun at once; the flashlight follows the
// camera that just moved.
void LightLabGame::onUpdate(World& world, const Input& input, float frameSeconds, float alpha) {
    // Motion from the previous mode must not rotate the new camera pose.
    if (!controls.updateModeSwitching(world, input)) {
        controls.updateCamera(world, input, frameSeconds, alpha);
    }

    updateDaylight(world);
    flashlight.place(world, flashlightInHand(controls.getCamera()));
    world.get<ProbeVolume>(probes) = probeSettings.denseProbes ? labDenseProbeVolume : labProbeVolume;
}

void LightLabGame::updateDaylight(World& world) {
    const Vec3 towardsSun = directionTowardsSunOn(sunSettings.latitudeDegrees, sunSettings.dayOfYear, sunSettings.solarHours);
    const Daylight daylight = daylightFor(towardsSun, sunSettings);
    world.get<DirectionalLight>(sun) = daylight.sun;
    world.get<AmbientLight>(sky) = daylight.sky;
    world.get<SkyBackground>(sky) = daylight.background;

    // The disc where the sunlight comes from, just inside the far plane, glowing with that light over its patch of sky.
    // Hidden once the sun has set (its light has faded to nothing).
    const Camera& camera = controls.getCamera();
    const float discAngularRadius = daylight.sun.angularRadius * sunSettings.sunDiscScale;
    world.get<Transform>(sunDisc) = skyObjectPose(camera.getPosition(), towardsSun, sunDiscDistanceShare * camera.getFarPlane(),
                                                  discAngularRadius);

    ModelRenderer& disc = world.get<ModelRenderer>(sunDisc);
    disc.visible = daylight.sun.intensity > 0.0f && discAngularRadius > 0.0f;
    disc.parts[0].material.emissive = discRadiance(daylight.sun.colour * daylight.sun.intensity, discAngularRadius);
}

Entity LightLabGame::getSunDisc() const {
    return sunDisc;
}

Entity LightLabGame::getFlashlight() const {
    return flashlight.getEntity();
}

Camera& LightLabGame::getCamera() {
    return controls.getCamera();
}

bool LightLabGame::wantsMouseLook() const {
    return controls.wantsMouseLook();
}

Vec3 LightLabGame::getLightFocus() {
    return controls.getLightFocus();
}

bool LightLabGame::wantsDebugView() const {
    return debugViewEnabled;
}

void LightLabGame::visitSettings(SettingsVisitor& visitor) {
    visitor.group("Sun", [&] {
        visit(sunSettings, visitor);
    });
    visitor.group("Probes", [&] {
        visit(probeSettings, visitor);
    });
    controls.visitSettings(visitor);
}

std::vector<GoldenView> LightLabGame::goldenViews() const {
    std::vector<GoldenView> views;

    for (const LabGoldenView& golden : labGoldenViews) {
        views.push_back(golden.view);
    }

    return views;
}

void LightLabGame::prepareGoldenView(World& world, const GoldenView& view) {
    for (const LabGoldenView& golden : labGoldenViews) {
        if (golden.view.name == view.name) {
            sunSettings.solarHours = golden.solarHours;
            updateDaylight(world);
            const Vec3 target = golden.facingSun
                ? golden.cameraPosition + directionTowardsSunOn(sunSettings.latitudeDegrees, sunSettings.dayOfYear, sunSettings.solarHours)
                : golden.cameraTarget;
            controls.poseFreeCamera(world, golden.cameraPosition, target);
            updateDaylight(world);
        }
    }
}

const StaticLevel& LightLabGame::getLevel() const {
    return staticLevel;
}

LightLabSunSettings& LightLabGame::getSunSettings() {
    return sunSettings;
}

LightLabProbeSettings& LightLabGame::getProbeSettings() {
    return probeSettings;
}

Entity LightLabGame::getSun() const {
    return sun;
}

Entity LightLabGame::getFirstPersonBody() const {
    return controls.getFirstPersonBody();
}

PlayerControls& LightLabGame::getControls() {
    return controls;
}
