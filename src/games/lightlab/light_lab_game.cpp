#include "games/lightlab/light_lab_game.h"

#include "assets/star_catalogue.h"
#include "games/lightlab/light_lab_level.h"
#include "gameplay/first_person_body.h"
#include "math/angles.h"
#include "core/srgb.h"
#include "math/transform.h"
#include "scene/celestial.h"
#include "scene/lighting.h"
#include "scene/model_renderer.h"
#include "scene/moon.h"
#include "scene/sky_object.h"
#include "scene/stars.h"
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

    // The field's grass, in sRGB like every material colour.
    const Pixel labGrassColour{75, 110, 75};

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
        case LabSurface::Asphalt:
            material.colour = Pixel{55, 55, 58};
            material.roughness = 0.9f;
            break;
        case LabSurface::Paving:
            material.colour = Pixel{150, 145, 135};
            material.roughness = 0.85f;
            break;
        case LabSurface::Brick:
            material.colour = Pixel{145, 80, 60};
            material.roughness = 0.9f;
            break;
        case LabSurface::Render:
            material.colour = Pixel{205, 195, 175};
            material.roughness = 0.85f;
            break;
        case LabSurface::Metal:
            material.colour = Pixel{70, 72, 75};
            material.metallic = 1.0f;
            material.roughness = 0.5f;
            break;
        case LabSurface::Glass:
            material.colour = Pixel{20, 24, 30};
            material.roughness = 0.1f;
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
    * red sun and long shadows), the sun itself, its disc and the glow bloom spreads round it, the dark back room
    * lit by the flashlight (a fast light: its bounce light kept in the probes' fast atlas), the back room lit by its
    * ceiling lamp (a still light), the moon over the field, and the street at night down its length, lit by its 60
    * lamps and its windows.
    */
    struct LabGoldenView {
        GoldenView view;
        Vec3 cameraPosition;
        Vec3 cameraTarget;
        float solarHours;

        // Look straight at the sun instead of at the target (which then is ignored), wherever it stands at that hour.
        bool facingSun = false;

        // The flashlight on (it starts off).
        bool flashlightOn = false;

        // The back room's lamp on (it starts off).
        bool lampOn = false;

        // The day of the year (the default's unless set), for the moon's phase.
        float dayOfYear = LightLabSunSettings{}.dayOfYear;

        // Look towards the moon's bearing instead of at the target, tilted down so the moon stands near the top of the
        // view and the ground shows below it.
        bool facingMoon = false;
    };

    // How far below the moon the moon-facing view looks.
    constexpr float moonViewBelowRadians = 0.45f;

    const LabGoldenView labGoldenViews[] = {
        {{"towers", 0.5, false}, Vec3{0.0f, 1.7f, 4.0f}, Vec3{0.0f, 3.0f, -40.0f}, 15.5f},
        {{"overview", 0.5, false}, Vec3{40.0f, 45.0f, 50.0f}, Vec3{-10.0f, 0.0f, -30.0f}, 15.5f},
        {{"sunlit-room", 0.5, false}, Vec3{-22.3f, 2.2f, 14.4f}, Vec3{-18.6f, 0.3f, 10.6f}, 15.5f},
        {{"sunset-hdr", 0.5, true}, Vec3{0.0f, 1.7f, 4.0f}, Vec3{0.0f, 3.0f, -40.0f}, 17.6f},
        {{"sun-disc", 0.5, false}, Vec3{0.0f, 1.7f, 4.0f}, Vec3{}, 15.5f, true},
        {{"flashlight", 0.5, false}, Vec3{-18.0f, 1.7f, 9.4f}, Vec3{-21.6f, 0.9f, 6.0f}, 15.5f, false, true},
        {{"back-room-lamp", 0.5, false}, Vec3{-17.7f, 1.7f, 9.5f}, Vec3{-21.5f, 1.2f, 5.8f}, 15.5f, false, false, true},
        {{"moonlit", 0.5, false}, Vec3{0.0f, 1.7f, 80.0f}, Vec3{}, 21.0f, false, false, false, 88.0f, true},
        {{"street-night", 0.5, false}, Vec3{-40.0f, 1.7f, 43.0f}, Vec3{40.0f, 3.0f, 46.0f}, 22.0f}
    };
}

void visit(LightLabSunSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Latitude", settings.latitudeDegrees, {.min = -90.0f, .max = 90.0f, .unit = "deg"});
    visitor.field("Year", settings.year, {.min = 1, .max = 9999, .sliderMin = 1900, .sliderMax = 2100});
    visitor.field("Day of year", settings.dayOfYear, {.min = 1.0f, .max = daysPerYear});
    visitor.field("Solar hours", settings.solarHours, {.min = 0.0f, .max = hoursPerDay, .unit = "h"});
    visitor.field("Hours per second", settings.hoursPerSecond, {.min = 0.0f, .max = hoursPerDay, .sliderMax = 1.0f, .unit = "h/s"});
    visitor.field("Sun intensity", settings.sunIntensity, {.min = 0.0f, .sliderMax = 10.0f});
    visitor.field("Sky brightness", settings.skyBrightness, {.min = 0.0f, .sliderMax = 5.0f});
    visitor.field("Atmosphere (real sky)", settings.atmosphere);
    visitor.field("Sun disc size", settings.sunDiscScale, {.min = 0.0f, .sliderMax = 10.0f, .unit = "x real"});
}


float advanceSolarHours(float solarHours, float hoursPerSecond, float seconds) {
    if (hoursPerSecond == 0.0f) {
        return solarHours;
    }

    const float advanced = std::fmod(solarHours + hoursPerSecond * seconds, hoursPerDay);
    return advanced < 0.0f ? advanced + hoursPerDay : advanced;
}

/*
* Whole days past midnight carry into the day of the year, and days past the year's last into the year. The clock never
* runs backwards (its speed is at least 0), so only forward carries happen.
*/
void advanceLabClock(LightLabSunSettings& settings, float seconds) {
    if (settings.hoursPerSecond == 0.0f) {
        return;
    }

    const float hours = settings.solarHours + settings.hoursPerSecond * seconds;
    const float days = std::floor(hours / hoursPerDay);
    settings.solarHours = hours - days * hoursPerDay;
    settings.dayOfYear += days;

    while (settings.dayOfYear >= daysPerYear + 1.0f) {
        settings.dayOfYear -= daysPerYear;
        ++settings.year;
    }
}

void visit(LightLabNightSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Moon intensity", settings.moonIntensity, {.min = 0.0f, .sliderMax = 1.0f});
    visitor.field("Moon disc size", settings.moonDiscScale, {.min = 0.0f, .sliderMax = 10.0f, .unit = "x real"});
    visitor.field("Star brightness", settings.starBrightness, {.min = 0.0f, .sliderMax = 20.0f, .unit = "millionths"});
    visitor.field("Star contrast", settings.starContrast, {.min = 0.0f, .max = 2.0f});
}

float starVisibility(const Vec3& towardsSun) {
    return 1.0f - smoothStep(radiansFromDegrees(labStarsFullBelowDegrees), radiansFromDegrees(labStarsAppearBelowDegrees), sunElevation(towardsSun));
}

DirectionalLight moonlightFor(const Vec3& towardsMoon, const Vec3& towardsSun, const LightLabNightSettings& settings, bool atmosphere) {
    DirectionalLight moonlight;
    moonlight.direction = towardsMoon * -1.0f;
    moonlight.colour = labMoonColour;
    moonlight.intensity = settings.moonIntensity * moonPhaseBrightness(moonPhaseAngle(towardsSun, towardsMoon));
    moonlight.castsShadows = true;
    moonlight.angularRadius = moonAngularRadius;
    moonlight.reflectsSun = true;

    if (!atmosphere) {
        moonlight.intensity *= smoothStep(radiansFromDegrees(sunFadeBelowDegrees), radiansFromDegrees(sunFullAboveDegrees), sunElevation(towardsMoon));
    }

    return moonlight;
}

Daylight daylightFor(const Vec3& towardsSun, const LightLabSunSettings& settings) {
    if (settings.atmosphere) {
        Daylight daylight;
        daylight.sun.direction = towardsSun * -1.0f;
        daylight.sun.colour = Vec3{1.0f, 1.0f, 1.0f};
        daylight.sun.intensity = settings.sunIntensity;
        daylight.sun.castsShadows = true;
        daylight.sky.colour = Vec3{0.0f, 0.0f, 0.0f}; // the sky's own light comes from the atmosphere
        return daylight;
    }

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
* triangles walked on, made from the same renderers, and the light probes move off them (ModelRenderer::isStatic).
*/
void LightLabGame::onInit(World& world, AssetManager& assets) {
    const MeshHandle cube = assets.addMesh(Mesh::cube());
    const LightLabLayout layout = buildLightLabLayout();

    Material grass;
    grass.colour = labGrassColour;
    grass.roughness = 1.0f;

    // The planet beyond the field is grass too, so its ground seen past the field's edge, and its light scattered back
    // up into the air, match the field (the albedo in linear light, as material colours are decoded).
    atmosphereSettings.groundAlbedo = Vec3{srgbByteToLinear(labGrassColour.r), srgbByteToLinear(labGrassColour.g), srgbByteToLinear(labGrassColour.b)};

    const auto addStatic = [&](ModelRenderer renderer) {
        renderer.isStatic = true;
        staticLevel.add(assets.makeCollisionMesh(renderer));
        const Entity entity = world.create();
        world.add(entity, Transform{});
        world.add(entity, std::move(renderer));
        return entity;
    };

    addStatic(makeMeshRenderer(assets.addMesh(Mesh::plane(labGroundHalfSize)), grass));
    addStatic(blocksRenderer(cube, layout.towers));
    addStatic(blocksRenderer(cube, layout.fence));
    addStatic(blocksRenderer(cube, layout.trees));
    addStatic(blocksRenderer(cube, layout.building));
    addStatic(blocksRenderer(cube, layout.street));

    // The windows, one part each in the layout's order, so switching finds each lit one's glow by its number.
    std::vector<LabBlock> panes;
    for (const LabWindow& window : layout.windows) {
        panes.push_back(window.pane);
    }
    streetWindows = addStatic(blocksRenderer(cube, panes));
    windowLayout = layout.windows;

    /*
    * The street lamps: a still light at each bulb while on, and the bulbs' glow as one renderer. Like the ceiling
    * lamp's panel the glow stands for the light's source, so it must not block that light; it never moves, so it is
    * static (visual only, it neither blocks the probes nor keeps them awake).
    */
    ModelRenderer bulbs;
    for (const Vec3& bulb : layout.streetLamps) {
        bulbs.parts.push_back(RenderPart{cube, surfaceMaterial(LabSurface::Glass),
                                         Mat4::translation(bulb.x, bulb.y, bulb.z) *
                                             Mat4::scaling(labStreetBulbHalfSize, labStreetBulbHalfSize, labStreetBulbHalfSize)});
        const Entity streetLamp = world.create();
        Transform placement;
        placement.position = bulb;
        world.add(streetLamp, placement);
        streetLamps.push_back(streetLamp);
    }
    bulbs.castsShadows = false;
    bulbs.isStatic = true;
    streetBulbs = world.create();
    world.add(streetBulbs, Transform{});
    world.add(streetBulbs, std::move(bulbs));

    controls.spawnBody(world);

    sun = world.create();
    world.add(sun, DirectionalLight{});
    moon = world.create();
    world.add(moon, DirectionalLight{});

    // The stars, made once and shared with every frame's copy of the sky.
    std::vector<Star> catalogue;
    for (const CatalogueStar& star : loadStarCatalogue(brightStarCataloguePath)) {
        catalogue.push_back(makeStar(star));
    }
    StarrySky starrySky;
    starrySky.stars = std::make_shared<const std::vector<Star>>(std::move(catalogue));
    stars = world.create();
    world.add(stars, std::move(starrySky));
    sky = world.create();
    world.add(sky, AmbientLight{});
    world.add(sky, SkyBackground{});
    air = world.create();
    probes = world.create();
    world.add(probes, ProbeCascades{});
    flashlight.create(world, false);

    // The lamp's panel stands for the bulb's glow, so like the sun disc it must not block the light it gives.
    ModelRenderer fixture;
    fixture.parts.push_back(RenderPart{cube, surfaceMaterial(LabSurface::Plaster),
                                       Mat4::translation(0.0f, labRoomHeight - labLampFixtureHalfSize.y - labLampPosition.y, 0.0f) *
                                           Mat4::scaling(labLampFixtureHalfSize.x, labLampFixtureHalfSize.y, labLampFixtureHalfSize.z)});
    fixture.castsShadows = false;
    lamp = world.create();
    Transform lampPlacement;
    lampPlacement.position = labLampPosition;
    world.add(lamp, lampPlacement);
    world.add(lamp, std::move(fixture));
    clockHoursSeen = sunSettings.solarHours;

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
    updateStreetLights(world);
}

void LightLabGame::onInput(World& world, const Input& input) {
    controls.updateFirstPersonCommands(world, input);

    if (input.wasKeyPressed(Key::F4)) {
        debugViewEnabled = !debugViewEnabled;
    }

    if (input.wasKeyPressed(Key::F)) {
        flashlight.toggle(world);
    }

    if (input.wasKeyPressed(Key::L)) {
        toggleLamp(world);
    }

    if (input.wasKeyPressed(Key::K)) {
        streetOverridden = !streetOverridden;
        updateStreetLights(world);
    }
}

bool streetLightsWantedFor(const Vec3& towardsSun) {
    return sunElevation(towardsSun) < radiansFromDegrees(labStreetLightsBelowDegrees);
}

/*
* The override is a flip of the automatic switch, not a state of its own, so it ends the moment the sun asks for
* something new: K at night turns the street off until dawn, K by day turns it on until dusk, and either way the street
* is back on the sun's clock afterwards.
*/
void LightLabGame::updateStreetLights(World& world) {
    const Vec3 towardsSun = directionTowardsSunOn(sunSettings.latitudeDegrees, sunSettings.dayOfYear, sunSettings.solarHours);
    const bool wanted = streetLightsWantedFor(towardsSun);

    if (wanted != streetWantedBySun) {
        streetWantedBySun = wanted;
        streetOverridden = false;
    }

    const bool on = wanted != streetOverridden;
    if (on != streetLit) {
        setStreetLights(world, on);
    }
}

/*
* Off removes the lights, as the ceiling lamp's switch does, so they take no seats or shadows by day. Sixty lamps
* switched together are one change to the light, so one event over the street and their reach round it.
*/
void LightLabGame::setStreetLights(World& world, bool on) {
    streetLit = on;

    for (const Entity streetLamp : streetLamps) {
        if (on) {
            world.add(streetLamp, labStreetLampLight);
        } else {
            world.remove<PointLight>(streetLamp);
        }
    }

    const Vec3 bulbGlow = on ? labStreetLampLight.colour * labStreetBulbGlow : Vec3{0.0f, 0.0f, 0.0f};
    for (RenderPart& bulb : world.get<ModelRenderer>(streetBulbs).parts) {
        bulb.material.emissive = bulbGlow;
    }

    std::vector<RenderPart>& panes = world.get<ModelRenderer>(streetWindows).parts;
    for (std::size_t index = 0; index < panes.size(); ++index) {
        const LabWindow& window = windowLayout[index];
        const Vec3 glow = labWarmWindowGlow + (labCoolWindowGlow - labWarmWindowGlow) * window.tint;
        panes[index].material.emissive = on && window.lit ? glow : Vec3{0.0f, 0.0f, 0.0f};
    }

    const float reach = labStreetLampLight.range;
    const Vec3 margin{reach, reach, reach};
    signalProbeEvent(world, ProbeEvent{ProbeEventKind::LightChange, false, Aabb{labStreetBounds.minimum - margin, labStreetBounds.maximum + margin}});
}

/*
* Off removes the light, as the flashlight's switch does, so it takes no seat; the panel glows only while it is on.
*/
void LightLabGame::toggleLamp(World& world) {
    const bool on = !world.has<PointLight>(lamp);

    if (on) {
        world.add(lamp, labLampLight);
    } else {
        world.remove<PointLight>(lamp);
    }

    world.get<ModelRenderer>(lamp).parts[0].material.emissive = on ? labLampLight.colour * labLampGlow : Vec3{0.0f, 0.0f, 0.0f};
    signalProbeEvent(world, probeEventAround(ProbeEventKind::LightChange, labLampPosition, labLampLight.range));
}

/*
* The clock's own ticks move clockHoursSeen along with it, so only a change from outside (the slider, a settings file)
* opens a gap; small drags add up until they pass the threshold. Measured the short way round the day.
*/
void LightLabGame::signalClockJumps(World& world) {
    const float gap = std::fmod(sunSettings.solarHours - clockHoursSeen + 1.5f * hoursPerDay, hoursPerDay) - 0.5f * hoursPerDay;

    if (std::abs(gap) > labClockJumpHours) {
        signalProbeEvent(world, ProbeEvent{ProbeEventKind::LightChange, true, Aabb{}});
        clockHoursSeen = sunSettings.solarHours;
    }
}

/*
* The clock advances in ticks, so the time of day depends only on how long the game has run, never on the frame rate.
*/
void LightLabGame::onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) {
    (void)simulationSeconds;
    advanceLabClock(sunSettings, tickSeconds);
    clockHoursSeen = advanceSolarHours(clockHoursSeen, sunSettings.hoursPerSecond, tickSeconds);
    updateFirstPersonBodies(world, staticLevel, tickSeconds);
}

// The daylight follows the settings every frame, so dragging a slider moves the sun at once; the flashlight follows the
// camera that just moved.
void LightLabGame::onUpdate(World& world, const Input& input, float frameSeconds, float alpha) {
    // Motion from the previous mode must not rotate the new camera pose.
    if (!controls.updateModeSwitching(world, input)) {
        controls.updateCamera(world, input, frameSeconds, alpha);
    }

    signalClockJumps(world);
    updateDaylight(world);
    updateStreetLights(world);
    flashlight.place(world, flashlightInHand(controls.getCamera()));
}

void LightLabGame::updateDaylight(World& world) {
    const Vec3 towardsSun = directionTowardsSunOn(sunSettings.latitudeDegrees, sunSettings.dayOfYear, sunSettings.solarHours);
    const Daylight daylight = daylightFor(towardsSun, sunSettings);
    world.get<DirectionalLight>(sun) = daylight.sun;
    world.get<AmbientLight>(sky) = daylight.sky;
    world.get<SkyBackground>(sky) = daylight.background;

    // The moon and the stars where the sky has turned to at this moment.
    const double days = daysSinceJ2000(sunSettings.year, sunSettings.dayOfYear, sunSettings.solarHours);
    const Mat4 toWorld = equatorialToWorld(radiansFromDegrees(sunSettings.latitudeDegrees), siderealAngle(days, sunSettings.solarHours));
    const EclipticPosition moonPosition = moonEclipticPosition(days);
    const Vec3 towardsMoon = transformDirection(toWorld, eclipticToEquatorial(moonPosition.longitude, moonPosition.latitude));
    world.get<DirectionalLight>(moon) = moonlightFor(towardsMoon, towardsSun, nightSettings, sunSettings.atmosphere);

    StarrySky& starrySky = world.get<StarrySky>(stars);
    starrySky.toWorld = toWorld;
    starrySky.brightness = nightSettings.starBrightness * labStarBrightnessUnit * starVisibility(towardsSun);
    starrySky.contrast = nightSettings.starContrast;

    // The real sky's air, while it is on; its discs drawn the same size as the lab's own.
    if (sunSettings.atmosphere) {
        Atmosphere atmosphere = atmosphereSettings;
        atmosphere.sunDiscScale = sunSettings.sunDiscScale;
        atmosphere.moonDiscScale = nightSettings.moonDiscScale;

        if (world.has<Atmosphere>(air)) {
            world.get<Atmosphere>(air) = atmosphere;
        } else {
            world.add(air, atmosphere);
        }
    } else if (world.has<Atmosphere>(air)) {
        world.remove<Atmosphere>(air);
    }

    // The disc where the sunlight comes from, just inside the far plane, glowing with that light over its patch of sky.
    // Hidden once the sun has set (its light has faded to nothing).
    const Camera& camera = controls.getCamera();
    const float discAngularRadius = daylight.sun.angularRadius * sunSettings.sunDiscScale;
    world.get<Transform>(sunDisc) = skyObjectPose(camera.getPosition(), towardsSun, sunDiscDistanceShare * camera.getFarPlane(),
                                                  discAngularRadius);

    ModelRenderer& disc = world.get<ModelRenderer>(sunDisc);
    disc.visible = !sunSettings.atmosphere && daylight.sun.intensity > 0.0f && discAngularRadius > 0.0f;
    disc.parts[0].material.emissive = discRadiance(daylight.sun.colour * daylight.sun.intensity, discAngularRadius);
}

Entity LightLabGame::getSunDisc() const {
    return sunDisc;
}

Entity LightLabGame::getFlashlight() const {
    return flashlight.getEntity();
}

Entity LightLabGame::getLamp() const {
    return lamp;
}

const std::vector<Entity>& LightLabGame::getStreetLamps() const {
    return streetLamps;
}

Entity LightLabGame::getStreetBulbs() const {
    return streetBulbs;
}

Entity LightLabGame::getStreetWindows() const {
    return streetWindows;
}

bool LightLabGame::streetLightsAreOn() const {
    return streetLit;
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
    visitor.group("Night", [&] {
        visit(nightSettings, visitor);
    });
    visitor.group("Atmosphere", [&] {
        visit(atmosphereSettings, visitor);
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
            // The clock stands still, so the view's ticks do not move the sun off its hour.
            sunSettings.solarHours = golden.solarHours;
            sunSettings.dayOfYear = golden.dayOfYear;
            sunSettings.hoursPerSecond = 0.0f;
            updateDaylight(world);
            Vec3 target = golden.facingSun
                ? golden.cameraPosition + directionTowardsSunOn(sunSettings.latitudeDegrees, sunSettings.dayOfYear, sunSettings.solarHours)
                : golden.cameraTarget;

            if (golden.facingMoon) {
                const Vec3 towardsMoon = world.get<DirectionalLight>(moon).direction.normalized() * -1.0f;
                const float pitch = sunElevation(towardsMoon) - moonViewBelowRadians;
                const Vec3 bearing = Vec3{towardsMoon.x, 0.0f, towardsMoon.z}.normalized();
                target = golden.cameraPosition + bearing * std::cos(pitch) + Vec3{0.0f, std::sin(pitch), 0.0f};
            }
            controls.poseFreeCamera(world, golden.cameraPosition, target);
            updateDaylight(world);
            updateStreetLights(world);

            if (golden.flashlightOn && !flashlight.isOn(world)) {
                flashlight.toggle(world);
            }

            if (golden.lampOn && !world.has<PointLight>(lamp)) {
                toggleLamp(world);
            }
        }
    }
}

const StaticLevel& LightLabGame::getLevel() const {
    return staticLevel;
}

LightLabSunSettings& LightLabGame::getSunSettings() {
    return sunSettings;
}

LightLabNightSettings& LightLabGame::getNightSettings() {
    return nightSettings;
}

Entity LightLabGame::getMoon() const {
    return moon;
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
