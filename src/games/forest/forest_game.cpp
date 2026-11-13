#include "games/forest/forest_game.h"

#include "core/srgb.h"
#include "games/forest/forest_terrain.h"
#include "gameplay/first_person_body.h"
#include "math/transform.h"
#include "scene/light.h"
#include "scene/model_renderer.h"
#include "scene/probe_volume.h"
#include "scene/sun.h"

#include <utility>

namespace {
    // The weights texture's size: 512 texels over 400 m, one about every 0.8 m; the layers' height maps make the
    // borders between them finer than that.
    constexpr int weightsTextureSize = 512;

    // The start: on the path at the meadow's northern edge, looking north up it towards the clearing.
    const Vec2 startPlace{25.6f, 138.8f};

    Vec3 onGround(float x, float z, float above = 0.0f) {
        return Vec3{x, forestHeight(x, z) + above, z};
    }

    PlayerControlsSetup forestViews() {
        return PlayerControlsSetup{
            .freeCameraPosition = onGround(startPlace.x, startPlace.y, 1.7f),
            .freeCameraTarget = onGround(0.0f, 0.0f, 2.0f),
            .mobaLookPoint = onGround(startPlace.x, startPlace.y - 8.0f),
            .firstPersonFeet = onGround(startPlace.x, startPlace.y),
            .verticalFovDegrees = 70.0f,
            .nearPlane = 0.1f,
            .farPlane = 1000.0f
        };
    }

    /*
    * The golden views: the meadow by mid-morning looking up the path, and the clearing at dusk from its southern edge,
    * the mist lit low across the hollows.
    */
    struct ForestGoldenView {
        GoldenView view;
        Vec3 cameraPosition;
        Vec3 cameraTarget;
        float solarHours;
    };

    const ForestGoldenView forestGoldenViews[] = {
        {{"meadow", 0.5, false}, onGround(startPlace.x, startPlace.y, 1.7f), onGround(3.0f, 92.0f, 1.0f), 9.5f},
        {{"clearing-dusk", 0.5, false}, onGround(4.0f, 30.0f, 1.7f), onGround(0.0f, -20.0f, 2.0f), 17.8f}
    };
}

void visit(ForestSunSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Latitude", settings.latitudeDegrees, {.min = -90.0f, .max = 90.0f, .unit = "deg"});
    visitor.field("Day of year", settings.dayOfYear, {.min = 1.0f, .max = daysPerYear});
    visitor.field("Solar hours", settings.solarHours, {.min = 0.0f, .max = hoursPerDay, .unit = "h"});
    visitor.field("Hours per second", settings.hoursPerSecond, {.min = 0.0f, .max = hoursPerDay, .sliderMax = 1.0f, .unit = "h/s"});
    visitor.field("Sun intensity", settings.sunIntensity, {.min = 0.0f, .sliderMax = 10.0f});
    visitor.field("Sun disc size", settings.sunDiscScale, {.min = 0.0f, .sliderMax = 10.0f, .unit = "x real"});
}

void visit(ForestMistSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Mist", settings.enabled);
    visit(settings.fog, visitor);
}

void visit(ForestGroundSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Blend depth", settings.blendDepth, {.min = 0.0f, .max = 1.0f});
}

ForestGame::ForestGame(ControlMode startMode, bool debugModeSwitching, std::string assetFolder):
    assetFolder(std::move(assetFolder)),
    controls(forestViews(), startMode, debugModeSwitching)
{
}

const char* ForestGame::title() const {
    return "Forest";
}

/*
* The ground is one static entity: the triangles drawn are the triangles walked on, and the sky visibility grid's rays
* and the light probes see it. Its material holds the four layers and the weights painting them.
*/
void ForestGame::onInit(World& world, AssetManager& assets) {
    Material groundMaterial;

    for (std::size_t layer = 0; layer < forestGroundLayers.size(); ++layer) {
        groundMaterial.groundLayers.push_back(forestLayerMaterial(assets, assetFolder, forestGroundLayers[layer]));
        groundMaterial.groundRepeats[layer] = 2.0f * forestHalfSize / forestGroundLayers[layer].tileMetres;
    }

    groundMaterial.groundWeights = assets.addTexture(forestWeightsImage(weightsTextureSize));
    groundMaterial.groundBlendDepth = groundSettings.blendDepth;

    ModelRenderer renderer = makeMeshRenderer(assets.addMesh(forestTerrainMesh()), groundMaterial);
    renderer.isStatic = true;
    staticLevel.add(assets.makeCollisionMesh(renderer));
    ground = world.create();
    world.add(ground, Transform{});
    world.add(ground, std::move(renderer));

    // The planet beyond the land is forest too, so the ground seen past the edge, and its light scattered back up into
    // the air, match it: the leaf litter's colour, in linear light as material colours are decoded.
    const Pixel& litter = forestGroundLayers[forestLitterLayer].standIn;
    atmosphereSettings.groundAlbedo = Vec3{srgbByteToLinear(litter.r), srgbByteToLinear(litter.g), srgbByteToLinear(litter.b)};

    controls.spawnBody(world);
    sun = world.create();
    world.add(sun, DirectionalLight{});
    air = world.create();
    probes = world.create();
    world.add(probes, ProbeCascades{});

    controls.setControlMode(world, controls.getMode());
    updateDaylight(world);
}

void ForestGame::onInput(World& world, const Input& input) {
    controls.updateFirstPersonCommands(world, input);

    if (input.wasKeyPressed(Key::F4)) {
        debugViewEnabled = !debugViewEnabled;
    }
}

void ForestGame::onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) {
    (void)simulationSeconds;
    sunSettings.solarHours = advanceSolarHours(sunSettings.solarHours, sunSettings.hoursPerSecond, tickSeconds);
    updateFirstPersonBodies(world, staticLevel, tickSeconds);
}

// The daylight, mist and blend follow the settings every frame, so a slider or a set command changes them at once.
void ForestGame::onUpdate(World& world, const Input& input, float frameSeconds, float alpha) {
    // Motion from the previous mode must not rotate the new camera pose.
    if (!controls.updateModeSwitching(world, input)) {
        controls.updateCamera(world, input, frameSeconds, alpha);
    }

    updateDaylight(world);
}

void ForestGame::updateDaylight(World& world) {
    DirectionalLight& light = world.get<DirectionalLight>(sun);
    light.direction = directionTowardsSunOn(sunSettings.latitudeDegrees, sunSettings.dayOfYear, sunSettings.solarHours) * -1.0f;
    light.colour = Vec3{1.0f, 1.0f, 1.0f};
    light.intensity = sunSettings.sunIntensity;
    light.castsShadows = true;

    Atmosphere atmosphere = atmosphereSettings;
    atmosphere.sunDiscScale = sunSettings.sunDiscScale;

    if (world.has<Atmosphere>(air)) {
        world.get<Atmosphere>(air) = atmosphere;
    } else {
        world.add(air, atmosphere);
    }

    if (mistSettings.enabled) {
        if (world.has<Fog>(air)) {
            world.get<Fog>(air) = mistSettings.fog;
        } else {
            world.add(air, mistSettings.fog);
        }
    } else if (world.has<Fog>(air)) {
        world.remove<Fog>(air);
    }

    world.get<ModelRenderer>(ground).parts.front().material.groundBlendDepth = groundSettings.blendDepth;
}

Camera& ForestGame::getCamera() {
    return controls.getCamera();
}

// The free camera takes the pose over, so it stays until the player flies it away.
bool ForestGame::poseCamera(World& world, const Vec3& position, const Vec3& target) {
    controls.poseFreeCamera(world, position, target);
    return true;
}

bool ForestGame::wantsMouseLook() const {
    return controls.wantsMouseLook();
}

Vec3 ForestGame::getLightFocus() {
    return controls.getLightFocus();
}

bool ForestGame::wantsDebugView() const {
    return debugViewEnabled;
}

void ForestGame::visitSettings(SettingsVisitor& visitor) {
    visitor.group("Sun", [&] {
        visit(sunSettings, visitor);
    });
    visitor.group("Atmosphere", [&] {
        visit(atmosphereSettings, visitor);
    });
    visitor.group("Mist", [&] {
        visit(mistSettings, visitor);
    });
    visitor.group("Ground", [&] {
        visit(groundSettings, visitor);
    });
    controls.visitSettings(visitor);
}

std::vector<GoldenView> ForestGame::goldenViews() const {
    std::vector<GoldenView> views;

    for (const ForestGoldenView& golden : forestGoldenViews) {
        views.push_back(golden.view);
    }

    return views;
}

void ForestGame::prepareGoldenView(World& world, const GoldenView& view) {
    for (const ForestGoldenView& golden : forestGoldenViews) {
        if (golden.view.name == view.name) {
            // The clock stands still, so the view's ticks do not move the sun off its hour.
            sunSettings.solarHours = golden.solarHours;
            sunSettings.hoursPerSecond = 0.0f;
            controls.poseFreeCamera(world, golden.cameraPosition, golden.cameraTarget);
            updateDaylight(world);
        }
    }
}

const StaticLevel& ForestGame::getLevel() const {
    return staticLevel;
}

Entity ForestGame::getGround() const {
    return ground;
}

Entity ForestGame::getSun() const {
    return sun;
}

Entity ForestGame::getAir() const {
    return air;
}

PlayerControls& ForestGame::getControls() {
    return controls;
}

ForestSunSettings& ForestGame::getSunSettings() {
    return sunSettings;
}

ForestMistSettings& ForestGame::getMistSettings() {
    return mistSettings;
}

ForestGroundSettings& ForestGame::getGroundSettings() {
    return groundSettings;
}
