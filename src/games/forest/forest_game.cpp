#include "games/forest/forest_game.h"

#include "scene/interpolation.h"

#include "core/srgb.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/leaf_clusters.h"
#include "games/forest/structure_seams.h"
#include "games/forest/structures.h"
#include "games/forest/valley_strata.h"
#include "scene/indexed_mesh.h"
#include "scene/simplify_mesh.h"
#include "gameplay/first_person_body.h"
#include "math/angles.h"
#include "math/transform.h"
#include "scene/beacon.h"
#include "scene/light.h"
#include "scene/model_renderer.h"
#include "scene/planting_cells.h"
#include "scene/probe_volume.h"
#include "scene/impostor.h"
#include "scene/still_draws.h"
#include "scene/sun.h"
#include "scene/terrain.h"

#include <algorithm>
#include <limits>
#include <array>
#include <filesystem>
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
    constexpr std::uint32_t emergentVariantSeed = 0x454d'4752u;
    constexpr std::uint32_t midVariantSeed = 0x4d49'4447u;
    constexpr std::uint32_t palmVariantSeed = 0x5041'4c4du;

    // The day's hours for the golden views: mid-morning (the sun low in the east, light across the valley), and dusk.
    constexpr float morningHours = 9.5f;
    constexpr float duskHours = 17.8f;

    // The structures' coating's tint, multiplying its albedo: a little cool, as the concept sheets' charcoal.
    const Vec3 coatingTint{0.94f, 0.98f, 1.08f};

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

        material.groundProjection = GroundProjection::Axes;
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

    // Still (ModelRenderer::isStill): the valley never moves, but for its drifting rocks, which say otherwise.
    Entity addShape(World& world, MeshHandle mesh, const Material& material, const Transform& transform, bool isStatic = false) {
        ModelRenderer renderer = makeMeshRenderer(mesh, material).withStill(true);
        renderer.isStatic = isStatic;
        const Entity entity = world.create();
        world.add(entity, transform);
        world.add(entity, std::move(renderer));
        return entity;
    }

    // The seam map's tile: texels across (16 m at 2048 is 0.8 cm a texel, a groove about four) and its pattern's seed.
    constexpr int seamTexels = 2048;
    constexpr std::uint32_t seamSeed = 0x5345'414du;

    // The structures' tunable parts (ForestStructureSettings), applied when they change: the coating is the layered
    // material's one layer, its seams read in each facet's plane at the ground's normal scale.
    void applyStructureSettings(Material& material, const ForestStructureSettings& settings) {
        Material& coat = material.groundLayers.front();
        const Vec3 albedo = coatingTint * std::max(settings.coatingAlbedo, 0.0f);
        coat.colour = Pixel{linearToSrgbByte(albedo.x), linearToSrgbByte(albedo.y), linearToSrgbByte(albedo.z)};
        coat.roughness = settings.coatingRoughness;
        coat.metallic = 0.0f;
        material.groundRepeats[0] = 1.0f / std::max(settings.seamTile, 0.1f);
        material.normalScale = std::max(settings.seamDepth, 0.0f);
        material.facetTone = settings.facetTone;
        material.facetRoughness = settings.facetRoughness;
    }

    /*
    * The Bastion's concrete (games/forest/bastion), applied when its settings change: the layer's albedo and roughness,
    * the formwork's seams at the ground's normal scale and repeating every formworkTile metres; each face's own tone a
    * little apart, as pours of concrete differ.
    */
    constexpr float concreteFaceTone = 0.06f;
    constexpr float concreteFaceRoughness = 0.04f;

    void applyBastionSettings(Material& material, const ForestBastionSettings& settings) {
        Material& concrete = material.groundLayers.front();
        const std::uint8_t grey = linearToSrgbByte(std::max(settings.concreteAlbedo, 0.0f));
        concrete.colour = Pixel{grey, grey, grey};
        concrete.roughness = settings.concreteRoughness;
        concrete.metallic = 0.0f;
        material.groundRepeats[0] = 1.0f / std::max(settings.formworkTile, 0.1f);
        material.normalScale = std::max(settings.formworkDepth, 0.0f);
        material.facetTone = concreteFaceTone;
        material.facetRoughness = concreteFaceRoughness;
        // The slit windows' colours at their brightness, and their cells' pitch (only towers with a band strip show them).
        const float brightness = std::max(settings.windowBrightness, 0.0f);
        material.windowWarm = bastionWarmLight * brightness;
        material.windowCool = bastionCoolLight * brightness;
        material.windowPitch = std::max(settings.windowPitch, 0.5f);
    }

    // The light strips' glow: the cool colour at the strips' brightness, on a dark painted bar.
    void applyStripSettings(Material& material, const ForestBastionSettings& settings) {
        material.emissive = bastionCoolLight * std::max(settings.stripBrightness, 0.0f);
    }

    // A floodlight's intensity and reach.
    void applyFloodSettings(SpotLight& light, const ForestBastionSettings& settings) {
        light.intensity = std::max(settings.floodIntensity, 0.0f);
        light.range = std::max(settings.floodRange, 1.0f);
    }

    /*
    * An obstruction light's live settings: intensity and on-time on every one; the flash rate and the spill on the
    * Bastion's only (the far towers keep their own rates, and throw no light on their walls).
    */
    void applyBeaconSettings(Beacon& beacon, const ForestBastionSettings& settings, bool onBastion) {
        beacon.intensity = std::max(settings.beaconIntensity, 0.0f);
        beacon.flash.onSeconds = std::max(settings.beaconOnSeconds, 0.0f);

        if (onBastion) {
            beacon.flash.periodSeconds = settings.beaconFlashesPerMinute > 0.0f ? 60.0f / settings.beaconFlashesPerMinute : 0.0f;
            beacon.spill = std::max(settings.beaconSpill, 0.0f);
            beacon.spillRange = std::max(settings.beaconSpillRange, 0.01f);
        }
    }

    // The far towers' obstruction lights: each tower flashing between this many and this many more times a minute (FAA: 20-40).
    constexpr float farBeaconLeastFlashes = 20.0f;
    constexpr float farBeaconFlashRange = 20.0f;

    // The floodlights' cone (half-angles, degrees) and source size (metres): a wide wash, soft up close.
    constexpr float floodInnerDegrees = 18.0f;
    constexpr float floodOuterDegrees = 30.0f;
    constexpr float floodSourceRadius = 2.0f;

    /*
    * A structure's material (games/forest/structures, structure_seams): one layer with a seam map, read once in each
    * facet's own plane, painted everywhere by a strip of one texel (strata, so the mesh's texture coordinates stay the
    * facets' numbers), then the facets' own tone and roughness on top; its numbers are the apply function's to set.
    */
    Material seamMaterial(AssetManager& assets, SeamImages seams) {
        Material layer;
        layer.texture = assets.addTexture(std::move(seams.albedo));
        layer.normalTexture = assets.addTexture(std::move(seams.normal));

        ImageData everywhere{1, 1, {255, 0, 0, 0}, TextureEncoding::Linear};
        everywhere.storage = TextureStorage::Exact;
        Material material;
        material.groundLayers.push_back(layer);
        material.groundProjection = GroundProjection::Facets;
        material.groundPaint = GroundPaint::Strata;
        material.groundWeights = assets.addTexture(std::move(everywhere));
        return material;
    }

    // The Ring's stealth coating: the sawtooth seam map.
    Material structureCoating(AssetManager& assets, JobSystem& jobs, const ForestStructureSettings& settings) {
        Material material = seamMaterial(assets, seamImages(seamLayout(ForestStructureSettings{}.seamTile, seamSeed), seamTexels, jobs));
        applyStructureSettings(material, settings);
        return material;
    }

    // The Bastion's concrete: the formwork's seam map, made for the default tile (the setting scales it).
    Material bastionConcrete(AssetManager& assets, JobSystem& jobs, const ForestBastionSettings& settings) {
        Material material = seamMaterial(assets, seamImages(formworkLayout(ForestBastionSettings{}.formworkTile), seamTexels, jobs));
        applyBastionSettings(material, settings);
        return material;
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
    visitor.group("Floor mist", [&] {
        visitor.field("Floor mist", settings.floorEnabled);
        visit(settings.floor, visitor);
    });
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

void visit(ForestStructureSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Coating albedo", settings.coatingAlbedo, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.2f});
    visitor.field("Coating roughness", settings.coatingRoughness, {.min = 0.0f, .max = 1.0f});
    visitor.field("Facet tone", settings.facetTone, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.4f});
    visitor.field("Facet roughness", settings.facetRoughness, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.3f});
    visitor.field("Seam depth", settings.seamDepth, {.min = 0.0f, .max = 10.0f, .sliderMax = 3.0f});
    visitor.field("Seam tile", settings.seamTile, {.min = 1.0f, .max = 200.0f, .sliderMax = 64.0f, .unit = "m"});
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
    flashlight.create(world, flashlightSettings.on);
    air = world.create();
    buildGround(world, assets);
    buildShapes(world, assets);
    buildPlantingMaps();
    buildTrees(assets);
    buildLianas(assets);
    buildUnderstorey(assets);
    buildPalms(assets);
    buildGrass(world, assets);
    buildFloorCover(world, assets);

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
    // The field the GPU plants its cells round the camera from (scene/planting_cells.h), and the trees' looks it draws
    // over the patch (given again whenever the sway changes, applySway).
    world.add(air, PlantingSource{std::make_shared<const PlantingField>(PlantingField{valleyMaps.planting, forestPlantingLayers(plantingSpecies())}), 1,
                                  plantedLooks()});
    floorMist = world.create();
    probes = world.create();
    world.add(probes, ProbeCascades{});

    controls.setControlMode(world, controls.getMode());
    updateDaylight(world);
    // Every near cell round the start at once, before the first frame.
    updateNearLianas(world, std::numeric_limits<int>::max());
}

/*
* The finest ring is the ground walked on and the one the sky visibility grid's rays see (static); the coarser rings are
* only drawn, and are not static: kilometres of them in the static level's 1 m cells would cost far more than the far
* ground's shading gains. Each ring has its own weights texture over its own square, blended the softer the coarser it is.
*/
void ForestGame::buildGround(World& world, AssetManager& assets) {
    const int coarseness = std::clamp(valleySettings.groundCoarseness, 0, 2);
    const std::vector<TerrainRing> rings = forestTerrainRings(static_cast<float>(2 << coarseness));

    Terrain land;

    for (std::size_t index = 0; index < rings.size(); ++index) {
        const GroundExtent extent{forestTerrainCentre, rings[index].outerHalfSize};
        const int weightsSize = (index == 0 ? nearWeightsSize : farWeightsSize) >> coarseness;
        const float texelMetres = 2.0f * extent.halfSize / static_cast<float>(weightsSize);
        const Material material = groundMaterial(assets, assetFolder, extent, weightsSize, forestBlendDepth(groundSettings.blendDepth, texelMetres), getJobs());
        land.regions.push_back(TerrainRegion{.centre = extent.centre, .halfSize = extent.halfSize, .material = material});
        groundTexels.push_back(texelMetres);

        // The finest ring stays the ground walked on and the sky visibility's static ground, but hidden: the terrain draws it.
        if (index == 0) {
            ground = addShape(world, assets.addMesh(forestTerrainRingMesh(rings[index], extent, getJobs())), material, Transform{}, true);
            ModelRenderer& hidden = world.get<ModelRenderer>(ground);
            hidden.hidden = true;
            hidden.castsShadows = false;
            staticLevel.add(assets.makeCollisionMesh(hidden));
        }
    }

    /*
    * The land drawn (scene/terrain.h): heights at the finest ring's spacing over its square, four times coarser over the
    * next ring's, sixteen times over everything, read from forestHeight as the rings were; leaves of 32 of the finest
    * squares, as many levels as reach past the land's edge from the centre.
    */
    const float nearSpacing = rings.front().spacing;
    auto heights = std::make_shared<TerrainHeights>();
    heights->grids.push_back(makeTerrainHeightGrid(forestTerrainCentre, rings[0].outerHalfSize, nearSpacing, forestHeight, getJobs()));
    heights->grids.push_back(makeTerrainHeightGrid(forestTerrainCentre, rings[1].outerHalfSize, 4.0f * nearSpacing, forestHeight, getJobs()));
    land.landHalfSize = rings.back().outerHalfSize;
    land.heights = heights;
    groundHeights = heights;
    land.centre = forestTerrainCentre;
    land.leafSize = nearSpacing * static_cast<float>(TERRAIN_GRID_QUADS);
    land.levels = 1;

    while (land.leafSize * static_cast<float>(1 << (land.levels - 1)) < 2.0f * land.landHalfSize) {
        ++land.levels;
    }

    const float rootHalf = 0.5f * land.leafSize * static_cast<float>(1 << (land.levels - 1));
    heights->grids.push_back(makeTerrainHeightGrid(forestTerrainCentre, rootHalf, 16.0f * nearSpacing, forestHeight, getJobs()));
    land.bounds = std::make_shared<const TerrainBounds>(makeTerrainBounds(land, getJobs()));
    land.grid = assets.addMesh(terrainGridMesh(TERRAIN_GRID_QUADS));
    land.halfGrid = assets.addMesh(terrainGridMesh(TERRAIN_HALF_GRID_QUADS));
    terrain = world.create();
    world.add(terrain, std::move(land));
}

/*
* A few pillar and island shapes, each placed many times turned and scaled, so the GPU draws all of one shape in one
* instanced draw. Only the Bastion and its bridge are static and walked against; the pillars, islands, Ring and far towers
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
    std::vector<std::vector<SimplifiedLevel>> shapeLevels(shapes.size());
    std::vector<IndexedMesh> moss(shapes.size());
    getJobs().parallelFor(shapes.size(), 1, [&](std::size_t index) {
        if (index < pillars.size()) {
            const SurfaceGrid surface = pillarSurface(pillars[index], carving);
            shapes[index] = wrappedSurfaceIndexedMesh(surface);
            shapeLevels[index] = simplifiedLevels(shapes[index], {rockLevelShare, rockFarLevelShare}, rockLevelError);
            moss[index] = pillarMoss(pillars[index], surface, mossSettings);
            return;
        }

        const IslandPlacement& island = islands[index - pillars.size()];
        const SurfaceGrid surface = islandSurface(island, carving);
        shapes[index] = wrappedSurfaceIndexedMesh(surface);
        shapeLevels[index] = simplifiedLevels(shapes[index], {rockLevelShare, rockFarLevelShare}, rockLevelError);
        IslandHangings& hung = hangings[index - pillars.size()];
        hung = islandHangings(island, surface, hangingSettings);
        moss[index] = islandMoss(island, surface, hung.tips, mossSettings, hung.tipArms);
    });

    const Material rock = rockMaterial(assets, assetFolder, rockSettings);

    for (std::size_t index = 0; index < shapes.size(); ++index) {
        const bool isPillar = index < pillars.size();
        const Vec3 place = isPillar ? pillars[index].foot : islands[index - pillars.size()].rim;
        const float turn = isPillar ? pillars[index].turnRadians : islands[index - pillars.size()].turnRadians;
        rockShapes.push_back(addShape(world, assets.addDrawnMeshWithLevels(std::move(shapes[index]), std::move(shapeLevels[index])), rock,
                                      Transform{.position = place, .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, turn)}));
    }

    // The roots and vines in the islands' own frames, placed and turned as their rock (none when the settings give none).
    const Material bark = forestLayerMaterial(assets, assetFolder, forestBarkLayer);
    const Material leaf = withLeafLight(forestLeafMaterial(assets, assetFolder), leafLightSettings);

    const Material mossMaterial = forestMossMaterial(assets, assetFolder);

    for (std::size_t index = 0; index < shapes.size(); ++index) {
        if (!moss[index].vertices.empty()) {
            mossEntities.push_back(addShape(world, assets.addDrawnMesh(std::move(moss[index])), mossMaterial, world.get<Transform>(rockShapes[index])));
            swayers.push_back(ForestSwayer{mossEntities.back(), ForestSwayPart::Hanging, 1.0f, false});
        }
    }

    for (std::size_t island = 0; island < islands.size(); ++island) {
        const Transform placed = world.get<Transform>(rockShapes[pillars.size() + island]);

        if (!hangings[island].bark.vertices.empty()) {
            hangingEntities.push_back(addShape(world, assets.addDrawnMesh(std::move(hangings[island].bark)), bark, placed));
            swayers.push_back(ForestSwayer{hangingEntities.back(), ForestSwayPart::Hanging, 1.0f, false});
        }

        if (!hangings[island].leaves.vertices.empty()) {
            hangingEntities.push_back(addShape(world, assets.addDrawnMesh(std::move(hangings[island].leaves)), leaf, placed));
            swayers.push_back(ForestSwayer{hangingEntities.back(), ForestSwayPart::Hanging, 1.0f, true});
        }
    }

    // The rocks drifting round the islands and some pillars, each its own shape at its size, built on every core. Their
    // rock is the shapes' painted in one bed's tones: beds by world height would slide over a rock that bobs.
    const std::vector<DriftingRock> drifting = valleyRocks(pillars, islands, driftSettings);
    std::vector<std::vector<SimplifiedLevel>> driftingLevels(drifting.size());
    std::vector<IndexedMesh> driftingShapes(drifting.size());
    getJobs().parallelFor(drifting.size(), 1, [&](std::size_t index) {
        driftingShapes[index] = wrappedSurfaceIndexedMesh(driftingRockSurface(drifting[index], drifting[index].seed));
        driftingLevels[index] = simplifiedLevels(driftingShapes[index], {rockLevelShare, rockFarLevelShare}, rockLevelError);
    });

    Material driftingRock = rock;
    driftingRock.groundWeights =
        assets.addTexture(strataImage({StrataBand{.bottom = valleyStrataBottom, .top = valleyStrataTop, .hard = false, .tone = 0.5f}}, 4));

    for (std::size_t index = 0; index < drifting.size(); ++index) {
        const Transform pose = driftingRockPose(drifting[index], 0.0);
        const Entity entity = addShape(world, assets.addDrawnMeshWithLevels(std::move(driftingShapes[index]), std::move(driftingLevels[index])), driftingRock, pose);
        world.add(entity, drifting[index]);
        world.add(entity, PreviousTransform{pose});
        setRenderStill(world, entity, false);
        driftingRocks.push_back(entity);
    }

    // The Ring (games/forest/structures), the alien relic: faceted, in a matte stealth coating whose facets each keep
    // their own tone. It stands across the path, its centre at the ground there, so its lower half is buried.
    const Material coating = structureCoating(assets, getJobs(), structureSettings);
    appliedStructureSettings = structureSettings;
    ring = addShape(world, assets.addMesh(ringSegmentsMesh(forestRingRadius, forestRingTubeRadius, forestShapeSeed(0, 43))), coating,
                    Transform{.position = onGround(forestRingPlace.x, forestRingPlace.y)});
    structures.push_back(ring);

    // The Bastion (games/forest/bastion) and its bridge to the pillar, static and walked against; the far towers from
    // the same kit, all in concrete with formwork.
    const Material concrete = bastionConcrete(assets, getJobs(), bastionSettings);
    appliedBastionSettings = bastionSettings;
    const auto addSolid = [&](Mesh mesh, const Transform& transform) {
        const Entity entity = addShape(world, assets.addMesh(std::move(mesh)), concrete, transform, true);
        staticLevel.add(assets.makeCollisionMesh(world.get<ModelRenderer>(entity)), transform.getMatrix());
        monolith.push_back(entity);
        structures.push_back(entity);
    };

    /*
    * At night (stage 4): each tower its own band strip of slit windows over its own heights (the bridge none), a mesh of
    * light strips on its major fins, and the Bastion its floodlights, unshadowed, washing its walls.
    */
    Material strip;
    strip.colour = Pixel{20, 22, 24};
    strip.roughness = 0.5f;
    applyStripSettings(strip, bastionSettings);

    const auto addTower = [&](const BastionShape& tower, bool solid) {
        Material material = concrete;
        material.windowBands = assets.addTexture(windowBandImage(tower, bastionWindowRows(tower), bastionSettings.windowsLit));
        material.strataBottom = tower.foot.y - tower.sink;
        material.strataTop = tower.foot.y + tower.headTop;
        const Transform place{.position = tower.foot};
        const Entity entity = addShape(world, assets.addMesh(bastionMesh(tower)), material, place, solid);

        if (solid) {
            staticLevel.add(assets.makeCollisionMesh(world.get<ModelRenderer>(entity)), place.getMatrix());
            monolith.push_back(entity);
        }

        structures.push_back(entity);
        lightStrips.push_back(addShape(world, assets.addMesh(bastionStripsMesh(tower)), strip, place));
    };

    const BastionShape bastion = monolithBastion(clearingLevel(), bastionSettings);
    addTower(bastion, true);
    addSolid(bridgeMesh(monolithBridge(clearingLevel(), bastion)), Transform{});

    for (const BastionShape& tower : farTowers()) {
        addTower(tower, false);
    }

    for (const FloodPlace& place : bastionFloods(bastion)) {
        SpotLight light{.colour = bastionFloodLight,
                        .direction = place.direction,
                        .innerAngle = radiansFromDegrees(floodInnerDegrees),
                        .outerAngle = radiansFromDegrees(floodOuterDegrees),
                        .sourceRadius = floodSourceRadius,
                        .castsShadows = false};
        applyFloodSettings(light, bastionSettings);
        const Entity flood = world.create();
        world.add(flood, Transform{.position = bastion.foot + place.position});
        world.add(flood, light);
        floodlights.push_back(flood);
    }

    /*
    * The obstruction lights (scene/beacon.h): the Bastion's flashing together and throwing a little red on its corners;
    * each far tower's at its own rate and phase, so the skyline blinks out of step, as towers on separate systems do.
    */
    const auto addBeacons = [&](const BastionShape& tower, const BeaconFlash& flash, bool onBastion) {
        for (const Vec3& place : bastionBeacons(tower, bastionSettings.beaconSpacing)) {
            Beacon beacon{.colour = bastionBeaconLight, .flash = flash};
            applyBeaconSettings(beacon, bastionSettings, onBastion);
            const Entity entity = world.create();
            world.add(entity, Transform{.position = tower.foot + place});
            world.add(entity, beacon);
            beacons.push_back(entity);
        }
    };

    addBeacons(bastion, BeaconFlash{}, true);
    bastionBeaconCount = beacons.size();
    const std::vector<BastionShape> far = farTowers();
    const std::uint32_t flashSeed = forestShapeSeed(0, 47);

    for (std::size_t index = 0; index < far.size(); ++index) {
        const float period = 60.0f / (farBeaconLeastFlashes + farBeaconFlashRange * forestUnitRandom(flashSeed, index));
        addBeacons(far[index], BeaconFlash{.periodSeconds = period, .phaseSeconds = period * forestUnitRandom(flashSeed, index + far.size())}, false);
    }
}

/*
* The planting maps (valley_maps) over the floor, walls and rim, before anything is planted: the obstacles from the
* pillars and the towers, a canopy cap for each species' reach under the sight lines.
*/
void ForestGame::buildPlantingMaps() {
    std::vector<BastionShape> towers = farTowers();
    towers.push_back(monolithBastion(clearingLevel(), bastionSettings));
    const std::vector<float> reaches{treeReach(emergentSettings), treeReach(treeSettings), treeReach(midSettings), palmReach(palmSettings),
                                     understoreyReach(understoreySettings)};
    valleyMaps = buildValleyMaps(plantingSettings, forestObstacles(pillars, towers), forestSightLines(), reaches, getJobs());
}

ForestSpecies ForestGame::plantingSpecies() const {
    return ForestSpecies{emergentSettings, treeSettings, midSettings, understoreySettings, palmSettings};
}

/*
* The variants are grown on every core, each into its own slot, and drawn only. Every tree of a variant shares its two
* meshes, so the GPU draws all of them in one instanced draw each; each is turned and scaled by its Transform. The
* leaves seen cast no shadow: their shadow leaves, fewer and bigger cards drawn into shadows only, cast it. Not
* static yet: the trunks and leaves join the sky visibility grid's rays with the forest floor.
*/
void ForestGame::buildTrees(AssetManager& assets) {
    // Every layer at once (forest_planting.h), on the land drawn: the understorey and the palms are built after the trees.
    const ForestPlanting planted = plantThePatch(plantingSpecies(), valleyMaps, *groundHeights, getJobs());
    emergentPlacements = planted.emergents;
    treePlacements = planted.canopy;
    midPlacements = planted.mid;
    understoreyPlacements = planted.understorey;
    palmPlacements = planted.palms;

    if (treePlacements.empty() && emergentPlacements.empty() && midPlacements.empty()) {
        return;
    }

    // Both species' leaves from one clusters' texture while their leaves and texels match (the defaults).
    const auto leafFor = [&](const ForestTreeSettings& settings) {
        return withLeafLight(forestLeafClusterMaterial(assets, assetFolder, forestTreeLeafTint, settings.clusterTexels, settings.leafLength / settings.clusterSize),
                             leafLightSettings);
    };
    const Material canopyLeaf = leafFor(treeSettings);
    const bool sameLeaves = emergentSettings.clusterTexels == treeSettings.clusterTexels &&
                            emergentSettings.leafLength / emergentSettings.clusterSize == treeSettings.leafLength / treeSettings.clusterSize;

    // The big trees' climbers (docs/research/VINES.md, stage 2): their stems in the islands' brown bark, their leaves drawn.
    const TreeClimbers climbers{.settings = &climberSettings,
                                .stem = forestLayerMaterial(assets, assetFolder, forestBarkLayer),
                                .leaf = withLeafLight(forestAroidLeafMaterial(assets, assetFolder), leafLightSettings)};

    // Every tree's epiphytes (docs/research/VINES.md, stage 3): the rosettes' strap leaves in their own atlas, the
    // limbs' moss from the moss scan; the mid layer's trees are small, carrying a few bromeliads.
    const TreeEpiphytes bigEpiphytes{.settings = &epiphyteSettings,
                                     .bigTree = true,
                                     .leaf = withLeafLight(forestEpiphyteLeafMaterial(assets, assetFolder), leafLightSettings),
                                     .moss = forestLayerMaterial(assets, assetFolder, forestLimbMossLayer)};
    TreeEpiphytes smallEpiphytes = bigEpiphytes;
    smallEpiphytes.bigTree = false;
    canopyHeights = plantTrees(assets, ForestLayer::Canopy, treeSettings, treePlacements, treeVariantSeed, canopyLeaf, &climbers, bigEpiphytes);
    emergentHeights = plantTrees(assets, ForestLayer::Emergent, emergentSettings, emergentPlacements, emergentVariantSeed,
                                 sameLeaves ? canopyLeaf : leafFor(emergentSettings), &climbers, bigEpiphytes);
    const bool midSameLeaves = midSettings.clusterTexels == treeSettings.clusterTexels &&
                               midSettings.leafLength / midSettings.clusterSize == treeSettings.leafLength / treeSettings.clusterSize;
    plantTrees(assets, ForestLayer::Mid, midSettings, midPlacements, midVariantSeed, midSameLeaves ? canopyLeaf : leafFor(midSettings), nullptr,
               smallEpiphytes);
}

namespace {
    // A scanned plant's mesh in the file's frame: its part's vertices placed by the part's transform (normals and
    // tangents turned by its normal matrix and made unit again).
    IndexedMesh placedPart(const ModelPart& part) {
        IndexedMesh mesh = buildIndexedMesh(*part.mesh);
        const Mat4 normals = normalMatrix(part.transform);

        for (MeshVertex& vertex : mesh.vertices) {
            vertex.position = transformPoint(part.transform, vertex.position);
            vertex.normal = transformDirection(normals, vertex.normal).normalized();
            const Vec3 tangent = transformDirection(part.transform, Vec3{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z}).normalized();
            vertex.tangent = Vec4{tangent.x, tangent.y, tangent.z, vertex.tangent.w};
        }

        return mesh;
    }

    // How far a leaf card's outline stands out from its twig's texels, as a share of the cell: 4 texels of a 1024 cell
    // (the mips' blur reaches past a leaf's edge).
    constexpr float clusterOutlineMargin = 4.0f / 1024.0f;

    // The stand-in plant's green.
    constexpr Pixel understoreyStandInColour{58, 92, 40};

    // One variant of a kind: its seen mesh, its shadow's stand-in and its material, and its card and the card's material.
    struct PlantVariant {
        MeshHandle leaves;
        MeshHandle shadow;
        Material material;
        MeshHandle card;
        Material cardMaterial;
        float height = 1.0f; // its seen mesh's top, which its sway bends against
    };

    /*
    * A plant's variant made ready to draw (docs/research/LOD.md): its seen mesh, with its coarser level and then its shadow's
    * stand-in as its levels, then nothing from impostorErrorMetres (or the stand-in's error, if larger) on, where its
    * card begins (scene/impostor.h, baked from the seen mesh as the trees' are from theirs, the texture copied first:
    * adding the atlases can move the asset manager's textures). All choose by the seen mesh's one box, so exactly one is
    * drawn. The card lets light through as the plant does. fades: its levels and card cross-fade by a dither rather than
    * swap (scene/mesh_level.h; docs/research/LOD.md), where its material is cut out, as the depth prepass discards there.
    */
    PlantVariant makePlantVariant(AssetManager& assets, JobSystem& jobs, IndexedMesh seen, IndexedMesh level, float levelError, IndexedMesh shadow,
                                  float shadowError, const Material& material, float impostorErrorMetres, int impostorFrames, int impostorTexels,
                                  bool fades) {
        const ImageData colour = material.texture.isValid() ? assets.getTexture(material.texture) : ImageData{};
        const ImpostorBake baked = bakeImpostor({ImpostorPart{.mesh = &seen,
                                                              .colour = material.texture.isValid() ? &colour : nullptr,
                                                              .tint = material.colour,
                                                              .alphaCutoff = isCutOut(material) ? material.alphaCutoff : 0.0f}},
                                                impostorFrames, impostorTexels, jobs);
        const Aabb box = indexedMeshBounds(seen);
        // A seen mesh thinned card by card (the palms' leaflets, scene/card_thinning.h) fades into its level, its own first
        // vertices, and then its card: the shadow's stand-in is never seen.
        const bool thinned = seen.thinning.cardSize > 0.0f;
        const float shadowLevelError = std::max(shadowError, levelError);
        const float impostorError = std::max(impostorErrorMetres, thinned ? levelError : shadowLevelError);

        const MeshHandle leaves = assets.addDrawnMesh(std::move(seen));
        const MeshHandle levelMesh = assets.addDrawnMesh(std::move(level));
        const MeshHandle shadowMesh = assets.addDrawnMesh(std::move(shadow));
        if (thinned) {
            assets.setMeshLevels(leaves, {MeshLevel{levelMesh, levelError, true}, MeshLevel{MeshHandle{}, impostorError}});
        } else {
            assets.setMeshLevels(leaves, {MeshLevel{levelMesh, levelError}, MeshLevel{shadowMesh, shadowLevelError}, MeshLevel{MeshHandle{}, impostorError}});
        }
        const bool fadesLevels = fades && isCutOut(material);
        assets.setMeshLevelOptions(leaves, MeshLevelOptions{.hiddenBelowError = 0.0f, .chooseBounds = box, .fadesLevels = fadesLevels});

        const MeshHandle card = assets.addDrawnMesh(impostorCardMesh(baked.centre, baked.radius));
        assets.setMeshLevelOptions(card, MeshLevelOptions{.hiddenBelowError = impostorError, .chooseBounds = box, .fadesLevels = fadesLevels});
        Material cardMaterial = impostorMaterial(assets.addTexture(baked.colour, impostorCoverageCutoff), assets.addTexture(baked.normal), impostorFrames);
        cardMaterial.roughness = material.roughness;
        cardMaterial.diffuseTransmission = material.diffuseTransmission;
        cardMaterial.diffuseTransmissionTint = material.diffuseTransmissionTint;
        return PlantVariant{leaves, shadowMesh, material, card, cardMaterial, std::max(box.maximum.y, 1e-3f)};
    }

    /*
    * A plant's variant as a look of the GPU's cells (scene/planting_cells.h): its seen mesh, casting no shadow; its
    * shadow's stand-in, drawn into the nearest shadowCascades only; and its card, drawn only far away, casting none (the
    * stand-in keeps its shadow). The first two sway as plants by sway from the start (and swayers, by their look's place
    * in looks, for applySway); the card, far off, does not.
    */
    template <typename LookSwayer>
    void addPlantLook(const PlantVariant& variant, int layer, int kind, int variantNumber, int shadowCascades, const ForestSwaySettings& sway,
                      std::vector<PlantedKindLook>& looks, std::vector<LookSwayer>& swayers) {
        PlantedKindLook look{.layer = layer, .kind = kind, .variant = variantNumber, .models = {}};
        ModelRenderer seen = makeMeshRenderer(variant.leaves, variant.material);
        seen.castsShadows = false;
        ModelRenderer shadow = makeMeshRenderer(variant.shadow, variant.material);
        shadow.shadowOnly = true;
        shadow.shadowCascades = shadowCascades;
        ModelRenderer card = makeMeshRenderer(variant.card, variant.cardMaterial);
        card.castsShadows = false;

        const ForestSwayer swayer{Entity{}, ForestSwayPart::Plants, variant.height, true};
        seen.sway = forestSway(sway, swayer);
        shadow.sway = seen.sway;

        for (std::size_t model = 0; model < 2; ++model) {
            swayers.push_back(LookSwayer{looks.size(), model, swayer});
        }

        look.models = {std::move(seen), std::move(shadow), std::move(card)};
        looks.push_back(std::move(look));
    }
}

void ForestGame::buildUnderstorey(AssetManager& assets) {
    if (understoreyPlacements.empty()) {
        return;
    }

    std::array<std::vector<PlantVariant>, understoreyKindCount> variants;

    for (int kind = 0; kind < understoreyKindCount; ++kind) {
        const std::string name = understoreyScanName(static_cast<UnderstoreyKind>(kind));
        const std::filesystem::path file = std::filesystem::path{assetFolder} / name / (name + "_1k.gltf");

        std::vector<IndexedMesh> plants;
        std::vector<Material> materials;

        if (std::filesystem::exists(file)) {
            for (const ModelPart& part : assets.loadModel(file.string()).parts) {
                plants.push_back(footedPlant(placedPart(part)));
                materials.push_back(withLeafLight(assets.makeMaterial(part.material), leafLightSettings));
            }
        }

        if (plants.empty()) {
            plants.push_back(understoreyStandIn());
            Material material = withLeafLight(plainMaterial(understoreyStandInColour, 0.7f), leafLightSettings);
            material.doubleSided = true;
            materials.push_back(material);
        }

        // The kind's tallest plant brought to its height, the others scaled alike: baked into the meshes.
        const float scale = understoreyKindScale(plants, understoreyHeight(understoreySettings, static_cast<UnderstoreyKind>(kind)));

        for (std::size_t plant = 0; plant < plants.size(); ++plant) {
            for (MeshVertex& vertex : plants[plant].vertices) {
                vertex.position = vertex.position * scale;
            }

            /*
            * The leaves seen keep their outline; their levels of detail (docs/research/LOD.md) let it go, as the shadow's stand-in
            * does, which is the coarsest. Both are simplified within the same error, so the first strays no further.
            */
            float levelError = 0.0f;
            float shadowError = 0.0f;
            IndexedMesh seen = simplifyMesh(plants[plant], understoreySettings.keepShare, understoreySimplifyError, true);
            IndexedMesh level = simplifyMesh(plants[plant], understoreySettings.levelKeepShare, understoreySimplifyError, false, &levelError);
            IndexedMesh shadow = simplifyMesh(plants[plant], understoreySettings.shadowKeepShare, understoreySimplifyError, false, &shadowError);
            variants[static_cast<std::size_t>(kind)].push_back(makePlantVariant(assets, getJobs(), std::move(seen), std::move(level), levelError,
                                                                                std::move(shadow), shadowError, materials[plant],
                                                                                understoreySettings.impostorErrorMetres, understoreySettings.impostorFrames,
                                                                                understoreySettings.impostorTexels, true));
        }
    }

    // Each kind's variants in order: the GPU draws the one a plant's pick chooses, as an entity chose (placedKindSlot).
    for (int kind = 0; kind < understoreyKindCount; ++kind) {
        const std::vector<PlantVariant>& each = variants[static_cast<std::size_t>(kind)];

        for (std::size_t variant = 0; variant < each.size(); ++variant) {
            addPlantLook(each[variant], static_cast<int>(ForestLayer::Understorey), kind, static_cast<int>(variant), understoreySettings.shadowCascades,
                         swaySettings, plantLooks, plantLookSwayers);
        }
    }
}

/*
* The variants grown on every core, each into its own slot, then made drawable in order (their cards baked, their
* meshes added) and placed; every palm of a variant shares its meshes, so each is one instanced draw.
*/
void ForestGame::buildPalms(AssetManager& assets) {
    if (palmPlacements.empty()) {
        return;
    }

    const Material leaf = withLeafLight(forestPalmLeafMaterial(assets, assetFolder), leafLightSettings);
    std::vector<PalmMeshes> grown(static_cast<std::size_t>(std::max(palmSettings.variants, 1)));
    getJobs().parallelFor(grown.size(), 1, [&](std::size_t variant) {
        grown[variant] = growPalmMeshes(palmVariantShape(palmSettings, static_cast<int>(variant), palmVariantSeed));
    });

    std::vector<PlantVariant> variants;

    for (PalmMeshes& meshes : grown) {
        variants.push_back(makePlantVariant(assets, getJobs(), std::move(meshes.palm), std::move(meshes.level), meshes.levelError, std::move(meshes.shadow),
                                            meshes.shadowError, leaf, palmSettings.impostorErrorMetres, palmSettings.impostorFrames,
                                            palmSettings.impostorTexels, false));
    }

    // A palm's variant is its kind in the layer.
    for (std::size_t variant = 0; variant < variants.size(); ++variant) {
        addPlantLook(variants[variant], static_cast<int>(ForestLayer::Palms), static_cast<int>(variant), 0, palmSettings.shadowCascades, swaySettings,
                     plantLooks, plantLookSwayers);
    }
}

namespace {
    // How far a kind of ground cover's levels reach (metres): half, quarter, nothing.
    struct CoverDistances {
        float half = 0.0f;
        float quarter = 0.0f;
        float farthest = 0.0f;
    };

    /*
    * A kind of ground cover placed (docs/research/GROUNDCOVER.md): every variant at every density step grown on every core
    * (grow(variant seed, density); a sparser step the first of the same pieces), then made drawable, each with its half
    * and quarter levels and nothing past the farthest. Each cell then draws its patch, turned its quarter turns about
    * its middle, lifted onto the land by the vertex shaders; casting no shadow (contact shadows and ambient occlusion
    * ground it), still. The patch type has mesh, level and farLevel. Each swaying as part (swayers), bending against its
    * patch's tallest plant.
    */
    // The least height ground cover bends against (m): a patch of fallen leaves alone, a centimetre high, would otherwise
    // bend and flutter as a whole plant does.
    constexpr float groundCoverLeastSwayHeight = 0.3f;

    template <typename Patch, typename Grow>
    void placeGroundCover(World& world, AssetManager& assets, JobSystem& jobs, const std::vector<GroundCell>& cells, int variantCount,
                          int stepCount, std::uint32_t seed, Grow grow, const Material& material, const CoverDistances& distances,
                          std::vector<Entity>& entities, std::vector<ForestSwayer>& swayers, ForestSwayPart part) {
        if (cells.empty()) {
            return;
        }

        const int variants = std::max(variantCount, 1);
        const int steps = std::max(stepCount, 1);
        std::vector<Patch> patches(static_cast<std::size_t>(variants * steps));
        jobs.parallelFor(patches.size(), 1, [&](std::size_t index) {
            const int variant = static_cast<int>(index) / steps;
            const int step = static_cast<int>(index) % steps + 1;
            patches[index] = grow(forestShapeSeed(seed, static_cast<std::uint32_t>(variant) + 1), static_cast<float>(step) / static_cast<float>(steps));
        });

        const float halfError = groundCoverLevelError(distances.half);
        const float quarterError = std::max(groundCoverLevelError(distances.quarter), halfError);
        const float farError = std::max(groundCoverLevelError(distances.farthest), quarterError);
        std::vector<MeshHandle> meshes;
        std::vector<float> heights;

        for (Patch& patch : patches) {
            heights.push_back(patch.mesh.vertices.empty() ? 1.0f : std::max(indexedMeshBounds(patch.mesh).maximum.y, groundCoverLeastSwayHeight));

            if (patch.mesh.vertices.empty()) {
                meshes.push_back(MeshHandle{});
                continue;
            }

            meshes.push_back(assets.addDrawnMesh(std::move(patch.mesh)));
            const MeshHandle level = patch.level.vertices.empty() ? MeshHandle{} : assets.addDrawnMesh(std::move(patch.level));
            const MeshHandle farLevel = patch.farLevel.vertices.empty() ? MeshHandle{} : assets.addDrawnMesh(std::move(patch.farLevel));
            assets.setMeshLevels(meshes.back(), {MeshLevel{level, halfError}, MeshLevel{farLevel, quarterError}, MeshLevel{MeshHandle{}, farError}});
        }

        entities.reserve(entities.size() + cells.size());

        for (const GroundCell& cell : cells) {
            const std::size_t patch = static_cast<std::size_t>(cell.variant * steps + cell.step - 1);
            const MeshHandle mesh = meshes[patch];

            if (!mesh.isValid()) {
                continue;
            }

            const Transform pose{.position = Vec3{cell.centre.x, 0.0f, cell.centre.y},
                                 .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 0.25f * fullTurnRadians * static_cast<float>(cell.quarterTurns))};
            entities.push_back(addShape(world, mesh, material, pose));
            ModelRenderer& renderer = world.get<ModelRenderer>(entities.back());
            renderer.castsShadows = false;
            renderer.followsGround = true;
            swayers.push_back(ForestSwayer{entities.back(), part, heights[patch], true});
        }
    }
}

void ForestGame::buildGrass(World& world, AssetManager& assets) {
    constexpr std::uint32_t grassPatchSeed = 0x50415443u;
    grassCells = forestGrassCells(grassSettings, getJobs());
    placeGroundCover<GrassPatch>(
        world, assets, getJobs(), grassCells, grassSettings.variants, grassSettings.densitySteps, grassPatchSeed,
        [&](std::uint32_t seed, float density) {
            return growGrassPatch(grassSettings, density, seed);
        },
        withLeafLight(forestGrassMaterial(assets, assetFolder), leafLightSettings),
        CoverDistances{grassSettings.halfDistance, grassSettings.quarterDistance, grassSettings.farthest}, grassEntities, swayers, ForestSwayPart::Grass);
}

void ForestGame::buildFloorCover(World& world, AssetManager& assets) {
    constexpr std::uint32_t floorPatchSeed = 0x464c5054u;
    floorCells = forestFloorCells(floorSettings, getJobs());
    placeGroundCover<FloorPatch>(
        world, assets, getJobs(), floorCells, floorSettings.variants, floorSettings.densitySteps, floorPatchSeed,
        [&](std::uint32_t seed, float density) {
            return growFloorPatch(floorSettings, density, seed);
        },
        withLeafLight(forestFloorCoverMaterial(assets, assetFolder), leafLightSettings),
        CoverDistances{floorSettings.halfDistance, floorSettings.quarterDistance, floorSettings.farthest}, floorEntities, swayers, ForestSwayPart::Floor);
}

namespace {
    // The lianas' own seed.
    constexpr std::uint64_t lianaSeed = 0x4c69616eull;

    /*
    * The lianas' template tube at its three levels (rings along, sides round), and how far each coarser one strays: 64
    * rings keep a 30 m liana's rings about half a metre apart as its meshes had them; 4 sides stray a 3 cm tube's radius
    * by 30% (about a centimetre), 3 sides and 6 rings sag off the curve by up to a few tens of centimetres.
    */
    constexpr int lianaNearRings = 64;
    constexpr int lianaNearSides = 6;
    constexpr int lianaMiddleRings = 16;
    constexpr int lianaMiddleSides = 4;
    constexpr float lianaMiddleError = 0.03f;
    constexpr int lianaFarRings = 6;
    constexpr int lianaFarSides = 3;
    constexpr float lianaFarError = 0.25f;

    // A tree variant's climbers' and epiphytes' uses of its seed (forestShapeSeed), apart from its growth's own uses.
    constexpr std::uint32_t climberSeedUse = 17;
    constexpr std::uint32_t epiphyteSeedUse = 18;
}

/*
* From the canopy and emergent trees' crowns (the mid layer's are too thin for these woody ones), over the valley's
* ground, more along the path, in the near cells round the player (near_trees.h): drawn by the GPU, one template tube
* bent onto each liana's record (scene/liana_shape.h) at three levels, bark as the trees', into the nearest cascades
* only. The cells within the radius of the start made at once; the rest follow the player (updateNearLianas).
*/
void ForestGame::buildLianas(AssetManager& assets) {
    const MeshHandle tube = assets.addDrawnMesh(lianaTemplateMesh(lianaNearRings, lianaNearSides));
    const MeshHandle middle = assets.addDrawnMesh(lianaTemplateMesh(lianaMiddleRings, lianaMiddleSides));
    const MeshHandle far = assets.addDrawnMesh(lianaTemplateMesh(lianaFarRings, lianaFarSides));
    assets.setMeshLevels(tube, {MeshLevel{middle, lianaMiddleError}, MeshLevel{far, lianaFarError}});
    lianaLook = NearLianaLook{.tube = tube,
                              .bark = forestLayerMaterial(assets, assetFolder, forestTreeBarkLayer),
                              .sway = forestSway(swaySettings, ForestSwayer{Entity{}, ForestSwayPart::Hanging, 1.0f, false}),
                              .shadowCascades = lianaShadowCascades};
}

/*
* The cells round the player: up to most made this frame (all within the radius at the start). Over the patch only,
* where the GPU draws the trees, until the whole valley is planted (ROADMAP e1).
*/
void ForestGame::updateNearLianas(World& world, int most) {
    const ForestSpecies species = plantingSpecies();
    const NearTreeSources sources{.species = species,
                                  .maps = valleyMaps.planting,
                                  .heights = *groundHeights,
                                  .region = forestPatch,
                                  .lianas = lianaSettings,
                                  .lianaSeed = lianaSeed};
    const NearLianaTrees trees{.emergentHeights = emergentHeights, .canopyHeights = canopyHeights, .bend = swaySettings.trees.bend};
    const Vec3 place = controls.getCamera().getPosition();
    nearLianas.update(world, air, Vec2{place.x, place.z}, nearTreeSettings, most, sources, trees, lianaLook, forestTreeBarkLayer.tileMetres, getJobs());
}

std::vector<float> ForestGame::plantTrees(AssetManager& assets, ForestLayer layer, const ForestTreeSettings& settings,
                                          const std::vector<TreePlacement>& placements, std::uint32_t variantSeed, const Material& leaf, const TreeClimbers* climbers, const TreeEpiphytes& epiphytes) {
    if (placements.empty()) {
        return {};
    }

    // Each cell of the leaf texture's outline, so the cards are drawn round their twigs, not as whole squares.
    ClusterOutlines outlines;

    if (leaf.texture.isValid()) {
        const ImageData& clusters = assets.getTexture(leaf.texture);
        const std::uint32_t cellTexels = clusters.width / static_cast<std::uint32_t>(clusterColumns);
        const std::uint8_t cutoff = static_cast<std::uint8_t>(std::clamp(leaf.alphaCutoff, 0.0f, 1.0f) * 255.0f);

        for (int cell = 0; cell < clusterColumns * clusterRows; ++cell) {
            outlines[static_cast<std::size_t>(cell)] = clusterOutline(clusters, static_cast<std::uint32_t>(cell % clusterColumns),
                                                                     static_cast<std::uint32_t>(cell / clusterColumns), cellTexels, cutoff,
                                                                     clusterOutlineMargin * static_cast<float>(cellTexels));
        }
    }

    const std::size_t variants = static_cast<std::size_t>(std::max(settings.variants, 1));
    std::vector<TreeMeshes> grown(variants);
    std::vector<ClimberMeshes> climbing(variants);
    std::vector<EpiphyteMeshes> perching(variants);
    getJobs().parallelFor(variants, 1, [&](std::size_t variant) {
        const std::uint32_t seed = forestShapeSeed(variantSeed, static_cast<std::uint32_t>(variant) + 1);
        grown[variant] = growTreeMeshes(settings, seed, forestTreeBarkLayer.tileMetres, &outlines);

        if (climbers != nullptr) {
            climbing[variant] = growClimbers(grown[variant].trunk, *climbers->settings, forestShapeSeed(seed, climberSeedUse));
        }

        perching[variant] = growEpiphytes(grown[variant].skeleton, *epiphytes.settings, epiphytes.bigTree, forestShapeSeed(seed, epiphyteSeedUse));
    });

    std::vector<MeshHandle> barkMeshes;
    std::vector<MeshHandle> leafMeshes;
    std::vector<MeshHandle> shadowLeafMeshes;
    std::vector<MeshHandle> shadowBarkMeshes;
    std::vector<MeshHandle> cardMeshes;
    std::vector<Material> cardMaterials;
    const Material bark = forestLayerMaterial(assets, assetFolder, forestTreeBarkLayer);
    // Copies, not references: adding each variant's atlases to the asset manager can move its textures.
    const ImageData barkTexture = bark.texture.isValid() ? assets.getTexture(bark.texture) : ImageData{};
    const ImageData leafTexture = leaf.texture.isValid() ? assets.getTexture(leaf.texture) : ImageData{};
    const ImageData climberStemTexture = climbers != nullptr && climbers->stem.texture.isValid() ? assets.getTexture(climbers->stem.texture) : ImageData{};
    const ImageData climberLeafTexture = climbers != nullptr && climbers->leaf.texture.isValid() ? assets.getTexture(climbers->leaf.texture) : ImageData{};
    const ImageData epiphyteLeafTexture = epiphytes.leaf.texture.isValid() ? assets.getTexture(epiphytes.leaf.texture) : ImageData{};
    const ImageData epiphyteMossTexture = epiphytes.moss.texture.isValid() ? assets.getTexture(epiphytes.moss.texture) : ImageData{};
    std::vector<MeshHandle> climberStemMeshes(grown.size());
    std::vector<MeshHandle> climberLeafMeshes(grown.size());
    std::vector<Vec3> climberFacing(grown.size());
    std::vector<MeshHandle> epiphyteLeafMeshes(grown.size());
    std::vector<MeshHandle> epiphyteMossMeshes(grown.size());
    std::vector<float> treeHeights(grown.size());
    std::vector<Vec3> firstRosette(grown.size());

    for (std::size_t index = 0; index < grown.size(); ++index) {
        TreeMeshes& variant = grown[index];
        ClimberMeshes& climber = climbing[index];
        const bool climbed = climbers != nullptr && !climber.stems.vertices.empty() && !climber.leaves.vertices.empty();
        /*
        * The impostor (scene/impostor.h), baked from the bark's first level and the leaves before they go to the asset
        * manager: its atlases, its card round the tree's sphere. The tree's parts end on a level drawing nothing at its
        * error, where the card begins, and all three choose by the tree's one box, so exactly one of them is drawn.
        */
        std::vector<ImpostorPart> parts{
            ImpostorPart{.mesh = &variant.barkLevel, .colour = bark.texture.isValid() ? &barkTexture : nullptr, .tint = bark.colour, .alphaCutoff = 0.0f},
            ImpostorPart{.mesh = &variant.leaves, .colour = leaf.texture.isValid() ? &leafTexture : nullptr, .tint = leaf.colour, .alphaCutoff = leaf.alphaCutoff}};

        // The climbers are on the card too, so the tree does not lose them far off.
        if (climbed) {
            parts.push_back(ImpostorPart{.mesh = &climber.stems, .colour = climbers->stem.texture.isValid() ? &climberStemTexture : nullptr,
                                         .tint = climbers->stem.colour, .alphaCutoff = 0.0f});
            parts.push_back(ImpostorPart{.mesh = &climber.leaves, .colour = climbers->leaf.texture.isValid() ? &climberLeafTexture : nullptr,
                                         .tint = climbers->leaf.colour, .alphaCutoff = 0.0f});
        }

        // The epiphytes too, rosettes and moss.
        EpiphyteMeshes& perched = perching[index];

        if (!perched.leaves.vertices.empty()) {
            parts.push_back(ImpostorPart{.mesh = &perched.leaves, .colour = epiphytes.leaf.texture.isValid() ? &epiphyteLeafTexture : nullptr,
                                         .tint = epiphytes.leaf.colour, .alphaCutoff = 0.0f});
        }

        if (!perched.moss.vertices.empty()) {
            parts.push_back(ImpostorPart{.mesh = &perched.moss, .colour = epiphytes.moss.texture.isValid() ? &epiphyteMossTexture : nullptr,
                                         .tint = epiphytes.moss.colour, .alphaCutoff = 0.0f});
        }

        const ImpostorBake baked = bakeImpostor(parts, settings.impostorFrames, settings.impostorTexels, getJobs());
        const float impostorError = std::max({settings.impostorErrorMetres, variant.shadowLeafError, variant.shadowBarkError});
        Aabb treeBox = enclosing(indexedMeshBounds(variant.bark), indexedMeshBounds(variant.leaves));

        if (climbed) {
            treeBox = enclosing(treeBox, enclosing(indexedMeshBounds(climber.stems), indexedMeshBounds(climber.leaves)));
        }

        for (const IndexedMesh* mesh : {&perched.leaves, &perched.moss}) {
            if (!mesh->vertices.empty()) {
                treeBox = enclosing(treeBox, indexedMeshBounds(*mesh));
            }
        }
        const MeshLevelOptions sharedBox{.hiddenBelowError = 0.0f, .chooseBounds = treeBox};
        treeHeights[index] = std::max(treeBox.maximum.y, 1e-3f);

        barkMeshes.push_back(assets.addDrawnMesh(std::move(variant.bark)));
        leafMeshes.push_back(assets.addDrawnMesh(std::move(variant.leaves)));
        shadowLeafMeshes.push_back(assets.addDrawnMesh(std::move(variant.shadowLeaves)));
        shadowBarkMeshes.push_back(assets.addDrawnMesh(std::move(variant.shadowBark)));
        cardMeshes.push_back(assets.addDrawnMesh(impostorCardMesh(baked.centre, baked.radius)));
        cardMaterials.push_back(impostorMaterial(assets.addTexture(baked.colour, impostorCoverageCutoff), assets.addTexture(baked.normal),
                                                 settings.impostorFrames));
        cardMaterials.back().roughness = leaf.roughness;
        cardMaterials.back().diffuseTransmission = leaf.diffuseTransmission;
        cardMaterials.back().diffuseTransmissionTint = leaf.diffuseTransmissionTint;

        // The levels of detail (docs/research/LOD.md): each seen mesh's coarser level, then the shadow's own stand-in, then nothing. The
        // leaves' coarser level is their own first vertices, thinned as the leaves are (scene/card_thinning.h).
        assets.setMeshLevels(barkMeshes.back(), {MeshLevel{assets.addDrawnMesh(std::move(variant.barkLevel)), variant.barkLevelError},
                                                 MeshLevel{shadowBarkMeshes.back(), variant.shadowBarkError}, MeshLevel{MeshHandle{}, impostorError}});
        assets.setMeshLevels(leafMeshes.back(), {MeshLevel{assets.addDrawnMesh(std::move(variant.leafLevel)), variant.leafLevelError, true},
                                                 MeshLevel{shadowLeafMeshes.back(), variant.shadowLeafError}, MeshLevel{MeshHandle{}, impostorError}});
        assets.setMeshLevelOptions(barkMeshes.back(), sharedBox);
        assets.setMeshLevelOptions(leafMeshes.back(), sharedBox);
        assets.setMeshLevelOptions(cardMeshes.back(), MeshLevelOptions{.hiddenBelowError = impostorError, .chooseBounds = treeBox});

        // The climbers drawn until the card takes over, chosen by the tree's box as its parts are.
        if (climbed) {
            climberFacing[index] = climber.facing;
            climberStemMeshes[index] = assets.addDrawnMesh(std::move(climber.stems));
            climberLeafMeshes[index] = assets.addDrawnMesh(std::move(climber.leaves));

            for (const MeshHandle mesh : {climberStemMeshes[index], climberLeafMeshes[index]}) {
                assets.setMeshLevels(mesh, {MeshLevel{MeshHandle{}, impostorError}});
                assets.setMeshLevelOptions(mesh, sharedBox);
            }
        }

        // The epiphytes likewise.
        if (!perched.sites.empty()) {
            firstRosette[index] = perched.sites.front().base;
        }

        for (auto [mesh, handle] : {std::pair{&perched.leaves, &epiphyteLeafMeshes[index]}, std::pair{&perched.moss, &epiphyteMossMeshes[index]}}) {
            if (!mesh->vertices.empty()) {
                *handle = assets.addDrawnMesh(std::move(*mesh));
                assets.setMeshLevels(*handle, {MeshLevel{MeshHandle{}, impostorError}});
                assets.setMeshLevelOptions(*handle, sharedBox);
            }
        }
    }

    /*
    * Each variant's parts, as one look (scene/planting_cells.h), which the GPU's cells draw the trees with. Bark, leaves
    * and card cast no shadow; the shadow's stand-ins (the shadow leaves and the shadow bark, one shadows-only model) cast
    * it; the climbers' stems and leaves and the epiphytes' rosettes and moss cast into the nearest cascades. All but the
    * card sway as trees (treeLookSwayers, applySway); the climbers' and epiphytes' models are listed for their tests.
    */
    for (std::size_t variant = 0; variant < grown.size(); ++variant) {
        PlantedKindLook look{.layer = static_cast<int>(layer), .kind = static_cast<int>(variant), .models = {}};
        const float height = treeHeights[variant];
        const auto seen = [&](MeshHandle mesh, const Material& material, bool castsShadows, int cascades, std::optional<bool> leafy) {
            ModelRenderer model = makeMeshRenderer(mesh, material);
            model.castsShadows = castsShadows;
            model.shadowCascades = cascades;

            if (leafy) {
                treeLookSwayers.push_back(LookSwayer{treeLooks.size(), look.models.size(), ForestSwayer{Entity{}, ForestSwayPart::Trees, height, *leafy}});
            }

            look.models.push_back(std::move(model));
        };
        seen(barkMeshes[variant], bark, false, 0, false);
        seen(leafMeshes[variant], leaf, false, 0, true);
        seen(cardMeshes[variant], cardMaterials[variant], false, 0, std::nullopt);
        seen(shadowLeafMeshes[variant], leaf, true, 0, true);
        look.models.back().shadowOnly = true;
        look.models.back().parts.push_back(RenderPart{shadowBarkMeshes[variant], bark});

        if (climberStemMeshes[variant].isValid()) {
            for (auto [mesh, material, leafy] : {std::tuple{climberStemMeshes[variant], &climbers->stem, false},
                                                 std::tuple{climberLeafMeshes[variant], &climbers->leaf, true}}) {
                climberModels.push_back(LookModel{treeLooks.size(), look.models.size()});
                seen(mesh, *material, true, climberShadowCascades, leafy);
            }
        }

        for (auto [mesh, material] : {std::pair{epiphyteLeafMeshes[variant], &epiphytes.leaf}, std::pair{epiphyteMossMeshes[variant], &epiphytes.moss}}) {
            if (mesh.isValid()) {
                epiphyteModels.push_back(LookModel{treeLooks.size(), look.models.size()});
                seen(mesh, *material, true, epiphyteShadowCascades, mesh == epiphyteLeafMeshes[variant]);
            }
        }

        treeLooks.push_back(std::move(look));
    }

    // The trees themselves are the GPU's (no entity each): only what the commands look from, the trees carrying climbers
    // and each one's first rosette, in the placements' order.
    for (const TreePlacement& tree : placements) {
        const Transform pose{.position = tree.foot, .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, tree.turnRadians),
                             .scale = Vec3{tree.scale, tree.scale, tree.scale}};
        const std::size_t variant = static_cast<std::size_t>(tree.variant);

        if (climberStemMeshes[variant].isValid()) {
            climbedTrees.push_back(tree);
            climbedFacing.push_back(pose.rotation.rotate(climberFacing[variant]));
        }

        if (epiphyteLeafMeshes[variant].isValid()) {
            const Vec3 rosette = transformPoint(pose.getMatrix(), firstRosette[variant]);
            const Vec3 out{rosette.x - tree.foot.x, 0.0f, rosette.z - tree.foot.z};
            epiphyteViews.push_back(rosette);
            epiphyteOutward.push_back(out.lengthSquared() > 1e-4f ? out.normalized() : pose.rotation.rotate(Vec3{1.0f, 0.0f, 0.0f}));
        }
    }

    return treeHeights;
}

void ForestGame::onInput(World& world, const Input& input) {
    controls.updateFirstPersonCommands(world, input);

    if (input.wasKeyPressed(Key::F4)) {
        debugViewEnabled = !debugViewEnabled;
    }

    if (input.wasKeyPressed(Key::F)) {
        flashlightSettings.on = !flashlightSettings.on;
    }
}

void ForestGame::onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) {
    updateDriftingRocks(world, simulationSeconds, driftSettings.moving);
    sunSettings.solarHours = advanceSolarHours(sunSettings.solarHours, sunSettings.hoursPerSecond, tickSeconds);
    updateFirstPersonBodies(world, staticLevel, tickSeconds);
    beaconClock = simulationSeconds;
    beaconTick = tickSeconds;
}

/*
* The daylight, haze and blend follow the settings every frame, so a slider or a set command changes them at once; the
* flashlight (gameplay/flashlight) follows the camera that just moved, and its setting, so F or a set command switches it.
* The obstruction lights flash by the simulation's clock between the last two ticks (the last tick's end is alpha 1),
* so the same moment always shows the same flash; only how they look changes, nothing a tick reads.
*/
void ForestGame::onUpdate(World& world, const Input& input, float frameSeconds, float alpha) {
    // Motion from the previous mode must not rotate the new camera pose.
    if (!controls.updateModeSwitching(world, input)) {
        controls.updateCamera(world, input, frameSeconds, alpha);
    }

    updateDaylight(world);
    updateNearLianas(world, nearTreeSettings.cellsAFrame);
    flashlight.place(world, flashlightInHand(controls.getCamera(), flashlightSettings));

    if (flashlight.isOn(world) != flashlightSettings.on) {
        flashlight.toggle(world);
    }

    flashBeacons(world, beaconClock - static_cast<double>(beaconTick) * (1.0 - static_cast<double>(alpha)));
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

    // Each layer of mist on its own entity, there while switched on.
    const auto placeFog = [&world](Entity entity, bool on, const Fog& fog) {
        if (on) {
            if (world.has<Fog>(entity)) {
                world.get<Fog>(entity) = fog;
            } else {
                world.add(entity, fog);
            }
        } else if (world.has<Fog>(entity)) {
            world.remove<Fog>(entity);
        }
    };
    placeFog(air, mistSettings.enabled, mistSettings.fog);
    placeFog(floorMist, mistSettings.enabled && mistSettings.floorEnabled, mistSettings.floor);

    // The wind over the valley (scene/wind.h), on the air entity too.
    if (world.has<Wind>(air)) {
        world.get<Wind>(air) = windSettings;
    } else {
        world.add(air, windSettings);
    }

    // The ground and the rock shapes are still: written, and announced, only when their settings changed.
    if (appliedGroundSettings != groundSettings) {
        Material& nearGround = world.get<ModelRenderer>(ground).parts.front().material;
        nearGround.groundBlendDepth = forestBlendDepth(groundSettings.blendDepth, groundTexels.front());
        nearGround.groundHexTiling = groundSettings.hexTiling;
        markRenderChanged(world, ground);
        appliedGroundSettings = groundSettings;
    }

    // The terrain's regions are the rings' materials, the finest the ground's own.
    std::vector<TerrainRegion>& regions = world.get<Terrain>(terrain).regions;

    for (std::size_t region = 0; region < regions.size(); ++region) {
        Material& regionMaterial = regions[region].material;
        regionMaterial.groundBlendDepth = forestBlendDepth(groundSettings.blendDepth, groundTexels[region]);
        regionMaterial.groundHexTiling = groundSettings.hexTiling;
    }

    if (appliedRockSettings != rockSettings) {
        for (const Entity shape : rockShapes) {
            applyRockSettings(world.get<ModelRenderer>(shape).parts.front().material, rockSettings);
            markRenderChanged(world, shape);
        }

        appliedRockSettings = rockSettings;
    }

    // The Ring's coating and the concrete of everything else, each rewritten only when its settings change.
    if (appliedStructureSettings != structureSettings) {
        applyStructureSettings(world.get<ModelRenderer>(ring).parts.front().material, structureSettings);
        markRenderChanged(world, ring);
        appliedStructureSettings = structureSettings;
    }

    if (appliedBastionSettings != bastionSettings) {
        for (const Entity structure : structures) {
            if (structure != ring) {
                applyBastionSettings(world.get<ModelRenderer>(structure).parts.front().material, bastionSettings);
                markRenderChanged(world, structure);
            }
        }

        for (const Entity strips : lightStrips) {
            applyStripSettings(world.get<ModelRenderer>(strips).parts.front().material, bastionSettings);
            markRenderChanged(world, strips);
        }

        for (const Entity flood : floodlights) {
            applyFloodSettings(world.get<SpotLight>(flood), bastionSettings);
        }

        for (std::size_t index = 0; index < beacons.size(); ++index) {
            applyBeaconSettings(world.get<Beacon>(beacons[index]), bastionSettings, index < bastionBeaconCount);
        }

        appliedBastionSettings = bastionSettings;
    }

    for (const Entity rock : driftingRocks) {
        applyRockSettings(world.get<ModelRenderer>(rock).parts.front().material, rockSettings);
    }

    applySway(world);
}

/*
* Everything swaying is still (ModelRenderer::isStill): given its sway, and announced, only when the settings changed.
* The placed trees' looks likewise, and given to the field anew.
*/
void ForestGame::applySway(World& world) {
    if (appliedSwaySettings == swaySettings) {
        return;
    }

    for (const ForestSwayer& swayer : swayers) {
        world.get<ModelRenderer>(swayer.entity).sway = forestSway(swaySettings, swayer);
        markRenderChanged(world, swayer.entity);
    }

    for (const LookSwayer& swaying : treeLookSwayers) {
        treeLooks[swaying.look].models[swaying.model].sway = forestSway(swaySettings, swaying.swayer);
    }

    for (const LookSwayer& swaying : plantLookSwayers) {
        plantLooks[swaying.look].models[swaying.model].sway = forestSway(swaySettings, swaying.swayer);
    }

    world.get<PlantingSource>(air).looks = plantedLooks();

    // The lianas sway as hanging things, their ends bending as their trees do (their records' bend).
    lianaLook.sway = forestSway(swaySettings, ForestSwayer{Entity{}, ForestSwayPart::Hanging, 1.0f, false});
    nearLianas.resway(world, air, lianaLook.sway, swaySettings.trees.bend);

    appliedSwaySettings = swaySettings;
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

bool ForestGame::setDebugView(bool on) {
    debugViewEnabled = on;
    return true;
}

void ForestGame::visitSettings(SettingsVisitor& visitor) {
    visitor.group("Sun", [&] {
        visit(sunSettings, visitor);
    });
    visitor.group("Atmosphere", [&] {
        visit(atmosphereSettings, visitor);
    });
    visitor.group("Wind", [&] {
        visit(windSettings, visitor);
    });
    visitor.group("Sway", [&] {
        visit(swaySettings, visitor);
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
    visitor.group("Structures", [&] {
        visit(structureSettings, visitor);
    });
    visitor.group("Bastion", [&] {
        visit(bastionSettings, visitor);
    });
    visitor.group("Flashlight", [&] {
        visit(flashlightSettings, visitor);
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
    visitor.group("Canopy trees", [&] {
        visit(treeSettings, visitor);
    });
    visitor.group("Emergent trees", [&] {
        visit(emergentSettings, visitor);
    });
    visitor.group("Mid trees", [&] {
        visit(midSettings, visitor);
    });
    visitor.group("Leaf light", [&] {
        visit(leafLightSettings, visitor);
    });
    visitor.group("Understorey", [&] {
        visit(understoreySettings, visitor);
    });
    visitor.group("Palms", [&] {
        visit(palmSettings, visitor);
    });
    visitor.group("Lianas", [&] {
        visit(lianaSettings, visitor);
    });
    visitor.group("Near trees", [&] {
        visit(nearTreeSettings, visitor);
    });
    visitor.group("Climbers", [&] {
        visit(climberSettings, visitor);
    });
    visitor.group("Epiphytes", [&] {
        visit(epiphyteSettings, visitor);
    });
    visitor.group("Grass", [&] {
        visit(grassSettings, visitor);
    });
    visitor.group("Forest floor", [&] {
        visit(floorSettings, visitor);
    });
    visitor.group("Valley", [&] {
        visit(valleySettings, visitor);
    });
    visitor.group("Planting", [&] {
        visit(plantingSettings, visitor);
    });
    controls.visitSettings(visitor);
}

namespace {
    // A command's optional tree number: 0 without one, nothing when there are more words or it is not a whole number.
    std::optional<std::size_t> treeNumber(const CommandArguments& arguments) {
        if (arguments.empty()) {
            return 0;
        }

        if (arguments.size() > 1) {
            return std::nullopt;
        }

        try {
            std::size_t used = 0;
            const std::size_t index = std::stoul(arguments.front(), &used);
            return used == arguments.front().size() ? std::optional<std::size_t>{index} : std::nullopt;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
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

    // Beside a tree carrying climbers (games/forest/climbers.h), to judge them: 7 m off its foot on its first climber's
    // side, eye high on the ground there, looking at its trunk 4 m up.
    registry.add("forest-climber", "forest-climber [n]", "Places the camera beside the n-th tree carrying climbers (the first without n).",
                 [this, &world](const CommandArguments& arguments) {
                     const std::optional<std::size_t> chosen = treeNumber(arguments);

                     if (!chosen) {
                         return commandError("Usage: forest-climber [n], n a tree number");
                     }

                     const std::size_t index = *chosen;

                     if (index >= climbedTrees.size()) {
                         return commandError(std::to_string(climbedTrees.size()) + " trees carry climbers, numbered from 0");
                     }

                     constexpr float distance = 7.0f;
                     constexpr float eyeHeight = 1.7f;
                     constexpr float lookHeight = 4.0f;
                     const Vec3& foot = climbedTrees[index].foot;
                     const Vec3 stand = foot + climbedFacing[index] * distance;
                     controls.poseFreeCamera(world, Vec3{stand.x, onGround(stand.x, stand.z).y + eyeHeight, stand.z}, foot + Vec3{0.0f, lookHeight, 0.0f});
                     return commandOk("Beside tree " + std::to_string(index) + " of " + std::to_string(climbedTrees.size()));
                 });

    // In the air beside a tree's first rosette (games/forest/epiphytes.h), to judge them: 8 m out from it, away from the
    // trunk, and 3 m above, looking at it.
    registry.add("forest-epiphyte", "forest-epiphyte [n]", "Places the camera in the air beside the n-th tree carrying epiphytes (the first without n).",
                 [this, &world](const CommandArguments& arguments) {
                     const std::optional<std::size_t> chosen = treeNumber(arguments);

                     if (!chosen) {
                         return commandError("Usage: forest-epiphyte [n], n a tree number");
                     }

                     if (*chosen >= epiphyteViews.size()) {
                         return commandError(std::to_string(epiphyteViews.size()) + " trees carry epiphytes, numbered from 0");
                     }

                     constexpr float distance = 8.0f;
                     constexpr float rise = 3.0f;
                     const Vec3& rosette = epiphyteViews[*chosen];
                     controls.poseFreeCamera(world, rosette + epiphyteOutward[*chosen] * distance + Vec3{0.0f, rise, 0.0f}, rosette);
                     return commandOk("Beside tree " + std::to_string(*chosen) + " of " + std::to_string(epiphyteViews.size()));
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

Entity ForestGame::getTerrain() const {
    return terrain;
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

ForestPlantingSettings& ForestGame::getPlantingSettings() {
    return plantingSettings;
}

const ValleyMaps& ForestGame::getValleyMaps() const {
    return valleyMaps;
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

ForestStructureSettings& ForestGame::getStructureSettings() {
    return structureSettings;
}

ForestBastionSettings& ForestGame::getBastionSettings() {
    return bastionSettings;
}

const std::vector<Entity>& ForestGame::getLightStrips() const {
    return lightStrips;
}

const std::vector<Entity>& ForestGame::getFloodlights() const {
    return floodlights;
}

const std::vector<Entity>& ForestGame::getBeacons() const {
    return beacons;
}

std::size_t ForestGame::getBastionBeaconCount() const {
    return bastionBeaconCount;
}

FlashlightSettings& ForestGame::getFlashlightSettings() {
    return flashlightSettings;
}

Entity ForestGame::getFlashlight() const {
    return flashlight.getEntity();
}

const std::vector<Entity>& ForestGame::getStructures() const {
    return structures;
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

ForestTreeSettings& ForestGame::getEmergentSettings() {
    return emergentSettings;
}

const std::vector<TreePlacement>& ForestGame::getEmergentPlacements() const {
    return emergentPlacements;
}

ForestTreeSettings& ForestGame::getMidSettings() {
    return midSettings;
}

const std::vector<TreePlacement>& ForestGame::getMidPlacements() const {
    return midPlacements;
}

ForestUnderstoreySettings& ForestGame::getUnderstoreySettings() {
    return understoreySettings;
}

const std::vector<UnderstoreyPlacement>& ForestGame::getUnderstoreyPlacements() const {
    return understoreyPlacements;
}

const std::vector<PlantedKindLook>& ForestGame::getTreeLooks() const {
    return treeLooks;
}

// Copies of the listed models, as the looks hold them now (their sway included).
std::vector<ModelRenderer> ForestGame::lookModels(const std::vector<LookModel>& listed) const {
    std::vector<ModelRenderer> models;
    models.reserve(listed.size());

    for (const LookModel& model : listed) {
        models.push_back(treeLooks.at(model.look).models.at(model.model));
    }

    return models;
}

ForestPalmSettings& ForestGame::getPalmSettings() {
    return palmSettings;
}

const std::vector<PalmPlacement>& ForestGame::getPalmPlacements() const {
    return palmPlacements;
}

const std::vector<PlantedKindLook>& ForestGame::getPlantLooks() const {
    return plantLooks;
}

// One list for the field: the trees' looks, then the plants' (applySway gives it anew when the sway changes).
std::shared_ptr<const PlantingLooks> ForestGame::plantedLooks() const {
    PlantingLooks looks{forestPatch, treeLooks};
    looks.kinds.insert(looks.kinds.end(), plantLooks.begin(), plantLooks.end());
    return std::make_shared<const PlantingLooks>(std::move(looks));
}

ForestLianaSettings& ForestGame::getLianaSettings() {
    return lianaSettings;
}

const std::vector<Entity>& ForestGame::getLianas() const {
    return nearLianas.getEntities();
}

ForestClimberSettings& ForestGame::getClimberSettings() {
    return climberSettings;
}

std::vector<ModelRenderer> ForestGame::getClimberModels() const {
    return lookModels(climberModels);
}

const std::vector<TreePlacement>& ForestGame::getClimbedTrees() const {
    return climbedTrees;
}

const std::vector<Vec3>& ForestGame::getClimbedFacing() const {
    return climbedFacing;
}

ForestEpiphyteSettings& ForestGame::getEpiphyteSettings() {
    return epiphyteSettings;
}

std::vector<ModelRenderer> ForestGame::getEpiphyteModels() const {
    return lookModels(epiphyteModels);
}

const std::vector<Vec3>& ForestGame::getEpiphyteViews() const {
    return epiphyteViews;
}

ForestGrassSettings& ForestGame::getGrassSettings() {
    return grassSettings;
}

const std::vector<GrassCell>& ForestGame::getGrassCells() const {
    return grassCells;
}

const std::vector<Entity>& ForestGame::getGrass() const {
    return grassEntities;
}

ForestFloorSettings& ForestGame::getFloorSettings() {
    return floorSettings;
}

Wind& ForestGame::getWindSettings() {
    return windSettings;
}

ForestSwaySettings& ForestGame::getSwaySettings() {
    return swaySettings;
}

const std::vector<GroundCell>& ForestGame::getFloorCells() const {
    return floorCells;
}

const std::vector<Entity>& ForestGame::getFloorCover() const {
    return floorEntities;
}
