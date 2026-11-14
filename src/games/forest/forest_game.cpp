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
    /*
    * The weights textures' sizes at the finest ground, one texture a ring over its own square: 2048 texels over the
    * walked-on ring's 1.28 km (one every 0.625 m; the layers' height maps make the borders finer still), then 1024 over
    * each coarser ring's square (5, 20 and 40 m), whose ground is that much farther away. One texture of 40 m texels for
    * every far ring drew rock patches as squares near the walk (the user's look at the rough valley). Halved for each
    * step of coarser ground, so a quick start stays quick.
    */
    constexpr int nearWeightsSize = 2048;
    constexpr int farWeightsSize = 1024;

    // The far plane: past the farthest ring of land (20.5 km from its centre), which reversed-Z depth keeps precise.
    constexpr float valleyFarPlane = 25000.0f;

    // The day's hours for the golden views: mid-morning (the sun low in the east, light across the valley), and dusk.
    constexpr float morningHours = 9.5f;
    constexpr float duskHours = 17.8f;

    // The grey stand-in materials, in sRGB as colour pickers give them: sandstone, the Monolith's concrete, the Ring's
    // steel.
    constexpr Pixel sandstone{178, 162, 138};
    constexpr Pixel concrete{122, 126, 130};
    constexpr Pixel steel{160, 164, 168};

    Vec3 onGround(float x, float z, float above = 0.0f) {
        return Vec3{x, forestHeight(x, z) + above, z};
    }

    // The clearing's level, where the Monolith stands.
    float clearingLevel() {
        return forestHeight(forestClearingCentre.x, forestClearingCentre.y);
    }

    PlayerControlsSetup forestControls() {
        const ForestView rim = forestViews().front();
        return PlayerControlsSetup{
            .freeCameraPosition = rim.position,
            .freeCameraTarget = rim.target,
            .mobaLookPoint = onGround(forestStartPlace.x, forestStartPlace.y - 8.0f),
            .firstPersonFeet = onGround(forestStartPlace.x, forestStartPlace.y),
            .verticalFovDegrees = 70.0f,
            .nearPlane = 0.1f,
            .farPlane = valleyFarPlane
        };
    }

    Material plainMaterial(Pixel colour, float roughness, float metallic = 0.0f) {
        Material material;
        material.colour = colour;
        material.roughness = roughness;
        material.metallic = metallic;
        return material;
    }

    // The ground's material over an extent: the four layers, each repeated by its tile size, painted by a weights texture.
    Material groundMaterial(AssetManager& assets, const std::string& assetFolder, const GroundExtent& extent, int weightsSize, float blendDepth,
                            JobSystem& jobs) {
        Material material;

        for (std::size_t layer = 0; layer < forestGroundLayers.size(); ++layer) {
            material.groundLayers.push_back(forestLayerMaterial(assets, assetFolder, forestGroundLayers[layer]));
            material.groundRepeats[layer] = 2.0f * extent.halfSize / forestGroundLayers[layer].tileMetres;
        }

        material.groundWeights = assets.addTexture(forestWeightsImage(weightsSize, extent, jobs));
        material.groundBlendDepth = blendDepth;
        return material;
    }

    Entity addShape(World& world, MeshHandle mesh, const Material& material, const Transform& transform, bool isStatic = false) {
        ModelRenderer renderer = makeMeshRenderer(mesh, material);
        renderer.isStatic = isStatic;
        const Entity entity = world.create();
        world.add(entity, transform);
        world.add(entity, std::move(renderer));
        return entity;
    }

    Transform boxTransform(const ValleyBox& box) {
        return Transform{.position = box.centre, .rotation = Quat{}, .scale = box.halfSize};
    }

    /*
    * The golden views: the concept art's plates. The valley from the rim by morning and at dusk, the Ring from the path,
    * and up the Monolith from its foot.
    */
    struct ForestGoldenView {
        GoldenView view;
        const char* place;
        float solarHours;
    };

    const ForestGoldenView forestGoldenViews[] = {
        {{"rim", 0.5, false}, "rim", morningHours},
        {{"ring", 0.5, false}, "ring", morningHours},
        {{"wall", 0.5, false}, "wall", morningHours},
        {{"rim-dusk", 0.5, false}, "rim", duskHours}
    };
}

std::vector<ForestView> forestViews() {
    const float level = clearingLevel();
    return {
        ForestView{"rim", onGround(forestStartPlace.x, forestStartPlace.y, 1.7f), Vec3{forestMonolithPlace.x, level + 220.0f, forestMonolithPlace.y}},
        ForestView{"ring", onGround(12.0f, 230.0f, 1.7f), onGround(forestRingPlace.x, forestRingPlace.y, 90.0f)},
        ForestView{"wall", Vec3{-150.0f, level + 1.7f, forestClearingCentre.y + 60.0f}, Vec3{60.0f, level + 220.0f, forestMonolithPlace.y}}
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

void visit(ForestValleySettings& settings, SettingsVisitor& visitor) {
    visitor.field("Ground coarseness", settings.groundCoarseness, {.min = 0, .max = 2, .applies = FieldApplies::AtStartup});
    visitor.field("Pillars", settings.pillarCount, {.min = 0, .max = 2000, .sliderMax = 600, .applies = FieldApplies::AtStartup});
    visitor.field("Floating islands", settings.islandCount, {.min = 0, .max = 500, .sliderMax = 100, .applies = FieldApplies::AtStartup});
}

ForestGame::ForestGame(ControlMode startMode, bool debugModeSwitching, std::string assetFolder):
    assetFolder(std::move(assetFolder)),
    controls(forestControls(), startMode, debugModeSwitching)
{
}

const char* ForestGame::title() const {
    return "Forest";
}

void ForestGame::onInit(World& world, AssetManager& assets) {
    buildGround(world, assets);
    buildShapes(world, assets);

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

/*
* The finest ring is the ground walked on and the one the sky visibility grid's rays see (static); the coarser rings are
* only drawn, and are not static: kilometres of them in the static level's 1 m cells would cost far more than the far
* ground's shading gains. Each ring has its own weights texture over its own square, blended the softer the coarser it is.
*/
void ForestGame::buildGround(World& world, AssetManager& assets) {
    const int coarseness = std::clamp(valleySettings.groundCoarseness, 0, 2);
    const std::vector<TerrainRing> rings = forestTerrainRings(static_cast<float>(2 << coarseness));

    for (std::size_t index = 0; index < rings.size(); ++index) {
        const GroundExtent extent{forestTerrainCentre, rings[index].outerHalfSize};
        const int weightsSize = (index == 0 ? nearWeightsSize : farWeightsSize) >> coarseness;
        const float texelMetres = 2.0f * extent.halfSize / static_cast<float>(weightsSize);
        const Material material = groundMaterial(assets, assetFolder, extent, weightsSize, forestBlendDepth(groundSettings.blendDepth, texelMetres), getJobs());
        const Entity entity = addShape(world, assets.addMesh(forestTerrainRingMesh(rings[index], extent, getJobs())), material, Transform{}, index == 0);
        groundTexels.push_back(texelMetres);

        if (index == 0) {
            ground = entity;
            staticLevel.add(assets.makeCollisionMesh(world.get<ModelRenderer>(ground)));
        } else {
            farGround.push_back(entity);
        }
    }
}

/*
* A few pillar and island shapes, each placed many times turned and scaled, so the GPU draws all of one shape in one
* instanced draw. Only the Monolith is static and walked against: a handful of big boxes; the pillars, islands and Ring
* stand clear of the path, so nobody walks into them yet.
*/
void ForestGame::buildShapes(World& world, AssetManager& assets) {
    const ValleyLayout layout{.pillarCount = valleySettings.pillarCount, .islandCount = valleySettings.islandCount};
    std::vector<MeshHandle> pillarShapes;
    std::vector<MeshHandle> islandShapes;

    for (int variant = 0; variant < layout.variants; ++variant) {
        pillarShapes.push_back(assets.addMesh(pillarMesh(static_cast<std::uint32_t>(variant))));
        islandShapes.push_back(assets.addMesh(islandMesh(static_cast<std::uint32_t>(variant))));
    }

    const Material rock = plainMaterial(sandstone, 0.9f);
    const auto addPillar = [&](const PillarPlacement& pillar) {
        addShape(world, pillarShapes[static_cast<std::size_t>(pillar.variant)], rock,
                 Transform{.position = pillar.foot, .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, pillar.turnRadians),
                           .scale = Vec3{pillar.radius, pillar.height, pillar.radius}});
    };

    pillars = placePillars(layout);
    pillars.push_back(monolithPillar(clearingLevel()));

    for (const PillarPlacement& pillar : pillars) {
        addPillar(pillar);
    }

    islands = placeIslands(layout, pillars);

    for (const IslandPlacement& island : islands) {
        addShape(world, islandShapes[static_cast<std::size_t>(island.variant)], rock,
                 Transform{.position = island.rim, .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, island.turnRadians),
                           .scale = Vec3{island.radius, island.radius, island.radius}});
    }

    // The Ring stands across the path, its centre at the ground there, so its lower half is buried.
    ring = addShape(world, assets.addMesh(ringMesh(forestRingTubeRadius / forestRingRadius)), plainMaterial(steel, 0.35f, 1.0f),
                    Transform{.position = onGround(forestRingPlace.x, forestRingPlace.y), .rotation = Quat{},
                              .scale = Vec3{forestRingRadius, forestRingRadius, forestRingRadius}});

    const MeshHandle box = assets.addMesh(Mesh::cube());
    const Material concreteMaterial = plainMaterial(concrete, 0.8f);

    for (const ValleyBox& part : monolithBoxes(clearingLevel())) {
        monolith.push_back(addShape(world, box, concreteMaterial, boxTransform(part), true));
        staticLevel.add(assets.makeCollisionMesh(world.get<ModelRenderer>(monolith.back())), boxTransform(part).getMatrix());
    }

    for (const ValleyBox& tower : farTowerBoxes()) {
        addShape(world, box, concreteMaterial, boxTransform(tower));
    }
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

// The daylight, haze and blend follow the settings every frame, so a slider or a set command changes them at once.
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

    world.get<ModelRenderer>(ground).parts.front().material.groundBlendDepth = forestBlendDepth(groundSettings.blendDepth, groundTexels.front());

    for (std::size_t far = 0; far < farGround.size(); ++far) {
        world.get<ModelRenderer>(farGround[far]).parts.front().material.groundBlendDepth =
            forestBlendDepth(groundSettings.blendDepth, groundTexels[far + 1]);
    }
}

bool ForestGame::showView(World& world, const std::string& name) {
    for (const ForestView& view : forestViews()) {
        if (name == view.name) {
            controls.poseFreeCamera(world, view.position, view.target);
            return true;
        }
    }

    return false;
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
    visitor.group("Valley", [&] {
        visit(valleySettings, visitor);
    });
    controls.visitSettings(visitor);
}

// forest-view: the concept art's places to look from, by name, through the free camera.
void ForestGame::addCommands(CommandRegistry& registry, World& world) {
    std::string names;

    for (const ForestView& view : forestViews()) {
        names += names.empty() ? view.name : std::string{", "} + view.name;
    }

    registry.add("forest-view", "forest-view <name>", "Places the camera at one of the valley's views: " + names + ".",
                 [this, &world, names](const CommandArguments& arguments) {
                     if (arguments.size() != 1) {
                         return commandError("Usage: forest-view <name>, one of " + names);
                     }

                     if (!showView(world, arguments.front())) {
                         return commandError("No view called " + arguments.front() + "; there are " + names);
                     }

                     return commandOk("Looking from " + arguments.front());
                 });
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
            showView(world, golden.place);
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

const std::vector<Entity>& ForestGame::getFarGround() const {
    return farGround;
}

const std::vector<PillarPlacement>& ForestGame::getPillars() const {
    return pillars;
}

const std::vector<IslandPlacement>& ForestGame::getIslands() const {
    return islands;
}

Entity ForestGame::getRing() const {
    return ring;
}

const std::vector<Entity>& ForestGame::getMonolith() const {
    return monolith;
}

ForestValleySettings& ForestGame::getValleySettings() {
    return valleySettings;
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
