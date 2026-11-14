#include "games/forest/forest_game.h"

#include "scene/interpolation.h"

#include "core/srgb.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/valley_strata.h"
#include "scene/indexed_mesh.h"
#include "gameplay/first_person_body.h"
#include "math/transform.h"
#include "scene/light.h"
#include "scene/model_renderer.h"
#include "scene/probe_volume.h"
#include "scene/sun.h"

#include <array>
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

    // The trees' variants' seeds.
    constexpr std::uint32_t treeVariantSeed = 0x4752'4f57u;

    // The day's hours for the golden views: mid-morning (the sun low in the east, light across the valley), and dusk.
    constexpr float morningHours = 9.5f;
    constexpr float duskHours = 17.8f;

    // The grey stand-in materials, in sRGB as colour pickers give them: the Monolith's concrete, the Ring's steel.
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

    // The strata strip's texels: 4096 over the cake's 1.8 km, about 0.44 m each, finer than the thinnest bed (5 m).
    constexpr int strataTexels = 4096;

    // The wavy beds' noise: 256 texels repeating every 1.5 km, so its four octaves rise and fall over 375 m and less.
    constexpr int strataWarpTexels = 256;
    constexpr float strataWarpTileMetres = 1500.0f;

    // The streaks' texture: 256 texels, one repeat every 24 m across and 300 m down, so a stain 3 m wide runs tens of
    // metres.
    constexpr int streakTexels = 256;
    constexpr float streakWidthMetres = 24.0f;
    constexpr float streakLengthMetres = 300.0f;

    // The tone variation's texture: 256 texels repeating every 400 m, so its patches are tens of metres across.
    constexpr int toneVariationTexels = 256;
    constexpr float toneVariationTileMetres = 400.0f;

    // The rock's tunable parts, applied each frame.
    void applyRockSettings(Material& material, const ForestRockSettings& settings) {
        material.groundBlendDepth = settings.blendDepth;
        material.groundTopStart = settings.mossStart;
        material.groundTopFull = settings.mossFull;
        material.streakStrength = settings.streakStrength;
        material.groundHexTiling = settings.hexTiling;
        material.variationStrength = settings.toneVariation;
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

    /*
    * The pillars' and islands' rock: the four rock layers projected along each shape's own axes, each repeating by its
    * tile size in metres, painted by the valley's strata (one strip by world height for every shape) with the moss on
    * what faces up.
    */
    Material rockMaterial(AssetManager& assets, const std::string& assetFolder, const ForestRockSettings& settings) {
        Material material;

        for (std::size_t layer = 0; layer < forestRockLayers.size(); ++layer) {
            material.groundLayers.push_back(forestLayerMaterial(assets, assetFolder, forestRockLayers[layer]));
            material.groundRepeats[layer] = 1.0f / forestRockLayers[layer].tileMetres;
        }

        material.groundProjected = true;
        material.groundPaint = GroundPaint::Strata;
        material.groundWeights = assets.addTexture(strataImage(valleyStrata(), strataTexels));
        material.strataBottom = valleyStrataBottom;
        material.strataTop = valleyStrataTop;
        material.groundTopLayer = forestMossLayer;
        material.strataWarp = assets.addTexture(strataWarpImage(strataWarpTexels));
        material.strataWarpTile = strataWarpTileMetres;
        material.strataWarpMetres = settings.warpMetres;
        material.groundStreaks = assets.addTexture(streakImage(streakTexels));
        material.streakWidth = streakWidthMetres;
        material.streakLength = streakLengthMetres;
        material.groundVariation = assets.addTexture(toneVariationImage(toneVariationTexels));
        material.variationTile = toneVariationTileMetres;
        applyRockSettings(material, settings);
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
    visitor.field("Hex tiling", settings.hexTiling);
}

void visit(ForestRockSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Blend depth", settings.blendDepth, {.min = 0.0f, .max = 1.0f});
    visitor.field("Moss starts", settings.mossStart, {.min = -1.0f, .max = 1.0f, .sliderMin = 0.0f});
    visitor.field("Moss full", settings.mossFull, {.min = -1.0f, .max = 1.0f, .sliderMin = 0.0f});
    visitor.field("Bed waves", settings.warpMetres, {.min = 0.0f, .max = 100.0f, .sliderMax = 20.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Streaks", settings.streakStrength, {.min = 0.0f, .max = 1.0f});
    visitor.field("Hex tiling", settings.hexTiling);
    visitor.field("Tone variation", settings.toneVariation, {.min = 0.0f, .max = 1.0f});
}

void visit(ForestValleySettings& settings, SettingsVisitor& visitor) {
    visitor.field("Ground coarseness", settings.groundCoarseness, {.min = 0, .max = 2, .applies = FieldApplies::AtStartup});
    visitor.field("Pillars", settings.pillarCount, {.min = 0, .max = 2000, .sliderMax = 600, .applies = FieldApplies::AtStartup});
    visitor.field("Floating islands", settings.islandCount, {.min = 0, .max = 500, .sliderMax = 100, .applies = FieldApplies::AtStartup});
    visitor.choice("Start", settings.start, std::array<const char*, 2>{"Rim", "Trees"}, FieldApplies::AtStartup);
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
    buildTrees(world, assets);

    // The planet beyond the land is forest too, so the ground seen past the edge, and its light scattered back up into
    // the air, match it: the leaf litter's colour, in linear light as material colours are decoded.
    const Pixel& litter = forestGroundLayers[forestLitterLayer].standIn;
    atmosphereSettings.groundAlbedo = Vec3{srgbByteToLinear(litter.r), srgbByteToLinear(litter.g), srgbByteToLinear(litter.b)};

    if (valleySettings.start == ForestStart::Trees) {
        const Vec2 start = forestTreesStart(treePlacements);
        controls.setFirstPersonFeet(onGround(start.x, start.y));
    }

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
    pillars = placePillars(layout);
    pillars.push_back(monolithPillar(clearingLevel()));
    islands = placeIslands(layout, pillars);

    // Each pillar and island its own shape, carved by the beds where they stand, built on every core (each into its own
    // slot, so the result is the same whatever order the jobs finish in) and kept only in the render format; each
    // island's roots and vines grown from its surface in the same job, and every shape's hanging moss.
    const RockCarving carving{.bands = valleyStrata(), .warp = strataWarpImage(strataWarpTexels), .warpTileMetres = strataWarpTileMetres,
                              .warpMetres = rockSettings.warpMetres};
    std::vector<IndexedMesh> shapes(pillars.size() + islands.size());
    std::vector<IslandHangings> hangings(islands.size());
    std::vector<IndexedMesh> moss(shapes.size());
    getJobs().parallelFor(shapes.size(), 1, [&](std::size_t index) {
        if (index < pillars.size()) {
            const SurfaceGrid surface = pillarSurface(pillars[index], carving);
            shapes[index] = wrappedSurfaceIndexedMesh(surface);
            moss[index] = pillarMoss(pillars[index], surface, mossSettings);
            return;
        }

        const IslandPlacement& island = islands[index - pillars.size()];
        const SurfaceGrid surface = islandSurface(island, carving);
        shapes[index] = wrappedSurfaceIndexedMesh(surface);
        IslandHangings& hung = hangings[index - pillars.size()];
        hung = islandHangings(island, surface, hangingSettings);
        moss[index] = islandMoss(island, surface, hung.tips, mossSettings);
    });

    const Material rock = rockMaterial(assets, assetFolder, rockSettings);

    for (std::size_t index = 0; index < shapes.size(); ++index) {
        const bool isPillar = index < pillars.size();
        const Vec3 place = isPillar ? pillars[index].foot : islands[index - pillars.size()].rim;
        const float turn = isPillar ? pillars[index].turnRadians : islands[index - pillars.size()].turnRadians;
        rockShapes.push_back(addShape(world, assets.addDrawnMesh(std::move(shapes[index])), rock,
                                      Transform{.position = place, .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, turn)}));
    }

    // The roots and vines in the islands' own frames, placed and turned as their rock (none when the settings give none).
    const Material bark = forestLayerMaterial(assets, assetFolder, forestBarkLayer);
    const Material leaf = forestLeafMaterial(assets, assetFolder);

    const Material mossMaterial = forestMossMaterial(assets, assetFolder);

    for (std::size_t index = 0; index < shapes.size(); ++index) {
        if (!moss[index].vertices.empty()) {
            mossEntities.push_back(addShape(world, assets.addDrawnMesh(std::move(moss[index])), mossMaterial, world.get<Transform>(rockShapes[index])));
        }
    }

    for (std::size_t island = 0; island < islands.size(); ++island) {
        const Transform placed = world.get<Transform>(rockShapes[pillars.size() + island]);

        if (!hangings[island].bark.vertices.empty()) {
            hangingEntities.push_back(addShape(world, assets.addDrawnMesh(std::move(hangings[island].bark)), bark, placed));
        }

        if (!hangings[island].leaves.vertices.empty()) {
            hangingEntities.push_back(addShape(world, assets.addDrawnMesh(std::move(hangings[island].leaves)), leaf, placed));
        }
    }

    // The rocks drifting round the islands and some pillars, each its own shape at its size, built on every core. Their
    // rock is the shapes' painted in one bed's tones: beds by world height would slide over a rock that bobs.
    const std::vector<DriftingRock> drifting = valleyRocks(pillars, islands, driftSettings);
    std::vector<IndexedMesh> driftingShapes(drifting.size());
    getJobs().parallelFor(drifting.size(), 1, [&](std::size_t index) {
        driftingShapes[index] = wrappedSurfaceIndexedMesh(driftingRockSurface(drifting[index], drifting[index].seed));
    });

    Material driftingRock = rock;
    driftingRock.groundWeights =
        assets.addTexture(strataImage({StrataBand{.bottom = valleyStrataBottom, .top = valleyStrataTop, .hard = false, .tone = 0.5f}}, 4));

    for (std::size_t index = 0; index < drifting.size(); ++index) {
        const Transform pose = driftingRockPose(drifting[index], 0.0);
        const Entity entity = addShape(world, assets.addDrawnMesh(std::move(driftingShapes[index])), driftingRock, pose);
        world.add(entity, drifting[index]);
        world.add(entity, PreviousTransform{pose});
        driftingRocks.push_back(entity);
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

/*
* The variants are grown on every core, each into its own slot, and drawn only. Every tree of a variant shares its two
* meshes, so the GPU draws all of them in one instanced draw each; each is turned and scaled by its Transform. Not
* static yet: the trunks and leaves join the sky visibility grid's rays with the forest floor.
*/
void ForestGame::buildTrees(World& world, AssetManager& assets) {
    treePlacements = placeTrees(treeSettings, pillars, forestSightLines());

    if (treePlacements.empty()) {
        return;
    }

    const std::size_t variants = static_cast<std::size_t>(std::max(treeSettings.variants, 1));
    std::vector<TreeMeshes> grown(variants);
    getJobs().parallelFor(variants, 1, [&](std::size_t variant) {
        grown[variant] = growTreeMeshes(treeSettings, forestShapeSeed(treeVariantSeed, static_cast<std::uint32_t>(variant) + 1),
                                        forestTreeBarkLayer.tileMetres);
    });

    std::vector<MeshHandle> barkMeshes;
    std::vector<MeshHandle> leafMeshes;

    for (TreeMeshes& variant : grown) {
        barkMeshes.push_back(assets.addDrawnMesh(std::move(variant.bark)));
        leafMeshes.push_back(assets.addDrawnMesh(std::move(variant.leaves)));
    }

    const Material bark = forestLayerMaterial(assets, assetFolder, forestTreeBarkLayer);
    const Material leaf = forestLeafClusterMaterial(assets, assetFolder, forestTreeLeafTint, treeSettings.clusterTexels,
                                                     treeSettings.leafLength / treeSettings.clusterSize);

    for (const TreePlacement& tree : treePlacements) {
        const Transform pose{.position = tree.foot, .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, tree.turnRadians),
                             .scale = Vec3{tree.scale, tree.scale, tree.scale}};
        const std::size_t variant = static_cast<std::size_t>(tree.variant);
        trees.push_back(addShape(world, barkMeshes[variant], bark, pose));
        trees.push_back(addShape(world, leafMeshes[variant], leaf, pose));
    }
}

void ForestGame::onInput(World& world, const Input& input) {
    controls.updateFirstPersonCommands(world, input);

    if (input.wasKeyPressed(Key::F4)) {
        debugViewEnabled = !debugViewEnabled;
    }
}

void ForestGame::onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) {
    updateDriftingRocks(world, simulationSeconds, driftSettings.moving);
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

    Material& nearGround = world.get<ModelRenderer>(ground).parts.front().material;
    nearGround.groundBlendDepth = forestBlendDepth(groundSettings.blendDepth, groundTexels.front());
    nearGround.groundHexTiling = groundSettings.hexTiling;

    for (std::size_t far = 0; far < farGround.size(); ++far) {
        Material& farMaterial = world.get<ModelRenderer>(farGround[far]).parts.front().material;
        farMaterial.groundBlendDepth = forestBlendDepth(groundSettings.blendDepth, groundTexels[far + 1]);
        farMaterial.groundHexTiling = groundSettings.hexTiling;
    }

    for (const Entity shape : rockShapes) {
        applyRockSettings(world.get<ModelRenderer>(shape).parts.front().material, rockSettings);
    }

    for (const Entity rock : driftingRocks) {
        applyRockSettings(world.get<ModelRenderer>(rock).parts.front().material, rockSettings);
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
    visitor.group("Rock", [&] {
        visit(rockSettings, visitor);
    });
    visitor.group("Hangings", [&] {
        visit(hangingSettings, visitor);
    });
    visitor.group("Moss", [&] {
        visit(mossSettings, visitor);
    });
    visitor.group("Drifting rocks", [&] {
        visit(driftSettings, visitor);
    });
    visitor.group("Trees", [&] {
        visit(treeSettings, visitor);
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

ForestRockSettings& ForestGame::getRockSettings() {
    return rockSettings;
}

const std::vector<Entity>& ForestGame::getRockShapes() const {
    return rockShapes;
}

ForestHangingSettings& ForestGame::getHangingSettings() {
    return hangingSettings;
}

const std::vector<Entity>& ForestGame::getIslandHangings() const {
    return hangingEntities;
}

ForestMossSettings& ForestGame::getMossSettings() {
    return mossSettings;
}

const std::vector<Entity>& ForestGame::getMoss() const {
    return mossEntities;
}

ForestDriftSettings& ForestGame::getDriftSettings() {
    return driftSettings;
}

const std::vector<Entity>& ForestGame::getDriftingRocks() const {
    return driftingRocks;
}

ForestTreeSettings& ForestGame::getTreeSettings() {
    return treeSettings;
}

const std::vector<TreePlacement>& ForestGame::getTreePlacements() const {
    return treePlacements;
}

const std::vector<Entity>& ForestGame::getTrees() const {
    return trees;
}
