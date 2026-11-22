#pragma once

#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "engine/game.h"
#include "games/forest/bastion.h"
#include "games/forest/drifting_rocks.h"
#include "games/forest/forest_ground.h"
#include "games/forest/forest_planting.h"
#include "games/forest/forest_trees.h"
#include "games/forest/hanging_moss.h"
#include "games/forest/island_hangings.h"
#include "games/forest/climbers.h"
#include "games/forest/epiphytes.h"
#include "games/forest/floor_cover.h"
#include "games/forest/grass.h"
#include "games/forest/lianas.h"
#include "games/forest/near_trees.h"
#include "games/forest/palms.h"
#include "games/forest/twigs.h"
#include "games/forest/understorey.h"
#include "games/forest/valley_maps.h"
#include "games/forest/valley_shapes.h"
#include "gameplay/control_mode.h"
#include "gameplay/flashlight.h"
#include "gameplay/player_controls.h"
#include "physics/static_level.h"
#include "scene/atmosphere.h"
#include "scene/fog.h"
#include "scene/planting_cells.h"
#include "scene/wind.h"
#include "games/forest/forest_sway.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

/*
* Where and when the forest's sun shines (scene/sun): near the equator, where lush forests grow, at the March equinox,
* mid-morning by default, so the light slants through the trees. The solar time is the clock, moving by itself at
* hoursPerSecond (0, still, by default). The sun's light is above the air: the atmosphere dims and reddens it on the way
* down and lights the scene with its sky.
*/
struct ForestSunSettings {
    float latitudeDegrees = 5.0f;
    float dayOfYear = 80.0f;
    float solarHours = 9.5f;
    float hoursPerSecond = 0.0f;
    float sunIntensity = 4.0f;
    float sunDiscScale = 5.0f;
};

void visit(ForestSunSettings& settings, SettingsVisitor& visitor);

/*
* The valley's mist, two layers of Fog that add (render/gpu/volumetric_fog.h, docs/research/FOGLAYERS.md):
* - fog, the haze: humid air filling the valley, about 6 km of sight on its floor (3 / density), thinning by e every
*   300 m up, so the pillars' tops and the islands stand clearer than their feet, out to 20 km along a ray (the far plane);
* - floor, a pool of mist on the valley's floor: about 100 m of sight (the user asked for a denser layer in the jungle
*   that tops out under the canopy, Nov 18 2026), even up to 12 m and fading over 4 m above, under the canopy trees'
*   forks (18 m); heights absolute, as cold air pools, so knolls rise out of it. Out to 2 km along a ray.
* enabled switches both; floorEnabled the pool alone.
*/
struct ForestMistSettings {
    bool enabled = true;
    Fog fog{.density = 0.0005f, .baseHeight = 0.0f, .scaleHeight = 300.0f, .maxDistance = 20000.0f};
    bool floorEnabled = false; // off by default (the user, Nov 19 2026); F5 Mist, Floor mist
    Fog floor{.density = 0.03f, .baseHeight = 12.0f, .scaleHeight = 4.0f, .maxDistance = 2000.0f, .shape = FogShape::Pooled};
};

void visit(ForestMistSettings& settings, SettingsVisitor& visitor);

// The ground's blend between its layers (Material::groundBlendDepth): shallow keeps borders crisp along the textures'
// own shapes, deep cross-fades them; and whether its layers are read by hex tiling, so their 2-3 m repeat does not show
// (off to compare).
struct ForestGroundSettings {
    float blendDepth = 0.2f;
    bool hexTiling = true;

    bool operator==(const ForestGroundSettings&) const = default;
};

void visit(ForestGroundSettings& settings, SettingsVisitor& visitor);

/*
* The pillars' and islands' rock (Material's projected strata): the blend between beds (shallow keeps their edges
* following the rocks' own shapes, deep cross-fades them), where the moss starts and is full, as the upward part of
* the surface's normal (1 level, 0 a wall), how far the beds rise and fall either way (at start-up: the shapes are carved
* by them), how dark the water streaks down the sides are (0 none, 1 black), whether the textures are read by hex tiling
* (so their repeat does not show; off to compare), and how much the rock's tone varies in patches tens of metres across.
*/
struct ForestRockSettings {
    float blendDepth = 0.3f;
    float mossStart = 0.55f;
    float mossFull = 0.8f;
    float warpMetres = 6.0f;
    float streakStrength = 0.45f;
    bool hexTiling = true;
    float toneVariation = 0.25f;

    bool operator==(const ForestRockSettings&) const = default;
};

void visit(ForestRockSettings& settings, SettingsVisitor& visitor);

/*
* The Ring's matte stealth coating (games/forest/structures, docs/research/STRUCTURES.md): its albedo (linear; charcoal
* paint is about 0.02-0.05, never 0, or the facets go black in shadow), its roughness, and how much each facet's own
* tone (a share) and roughness vary, so neighbouring facets at nearly the same angle stay apart. Its panel lines
* (games/forest/structure_seams): how deep the seams' normal map reads (its scale; 0 flat), and the seam tile's size in
* metres (the pattern scales with it).
*/
struct ForestStructureSettings {
    float coatingAlbedo = 0.04f;
    float coatingRoughness = 0.65f;
    float facetTone = 0.12f;
    float facetRoughness = 0.07f;
    float seamDepth = 1.0f;
    float seamTile = 16.0f;

    bool operator==(const ForestStructureSettings&) const = default;
};

void visit(ForestStructureSettings& settings, SettingsVisitor& visitor);

// Where the player starts: on the rim, seeing the valley's scale (the design's start), or among the trees on the path
// between the Ring and the clearing (for looking at them).
enum class ForestStart {
    Rim,
    Trees
};

/*
* What the valley is built with, read once at start-up: the finest ground squares (2 m, or 4 or 8 m for a quicker start:
* 2 << groundCoarseness), how many pillars and floating islands there are (fewer if they run out of room), and where the
* player starts.
*/
struct ForestValleySettings {
    int groundCoarseness = 0;
    int pillarCount = 220;
    int islandCount = 30;
    ForestStart start = ForestStart::Rim;
};

void visit(ForestValleySettings& settings, SettingsVisitor& visitor);

// A named place to look from (the concept art's plates), for the forest-view command and the golden views.
struct ForestView {
    const char* name;
    Vec3 position;
    Vec3 target;
};

// rim (the valley from the start), ring (the Ring from the path) and wall (up the Monolith from its foot).
std::vector<ForestView> forestViews();

// A tree look's leaves model (the leaf cards): the second, after the bark.
inline constexpr std::size_t treeLeavesModel = 1;

/*
* The jungle valley showcase (ROADMAP phase 18), so far the rough valley, its test of scale: the land in rings reaching
* 20 km (forest_terrain.h), its ground blended from scanned grass, leaf litter, mud and mossy rock (forest_ground.h; plain
* stand-ins until tools/fetch_forest_assets.py has fetched them), grey pillars, floating islands, the Ring, the Monolith
* and far towers at their real sizes (valley_shapes.h), the first grown trees (forest_trees.h), a sun near the equator
* through a real sky and the valley's haze.
* The same controls as the other games (F1 first person, F2 MOBA view, F3 free camera), and forest-view <name>.
*/
class ForestGame final : public Game {
public:
    explicit ForestGame(ControlMode startMode = ControlMode::FirstPerson, bool debugModeSwitching = true,
                        std::string assetFolder = forestAssetFolder);

    const char* title() const override;
    void onInit(World& world, AssetManager& assets) override;
    void onInput(World& world, const Input& input) override;
    void onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) override;
    void onUpdate(World& world, const Input& input, float frameSeconds, float alpha) override;
    Camera& getCamera() override;
    bool poseCamera(World& world, const Vec3& position, const Vec3& target) override;
    bool wantsMouseLook() const override;
    Vec3 getLightFocus() override;
    bool wantsDebugView() const override;
    bool setDebugView(bool on) override;
    void visitSettings(SettingsVisitor& visitor) override;
    void addCommands(CommandRegistry& registry, World& world) override;
    std::vector<GoldenView> goldenViews() const override;
    void prepareGoldenView(World& world, const GoldenView& view) override;

    // For tests.
    const StaticLevel& getLevel() const;
    Entity getGround() const;
    Entity getTerrain() const;
    const std::vector<PillarPlacement>& getPillars() const;
    const std::vector<IslandPlacement>& getIslands() const;
    Entity getRing() const;
    const std::vector<Entity>& getMonolith() const;
    ForestValleySettings& getValleySettings();
    // The planting maps' settings (read at start-up) and the maps every plant is planted from.
    ForestPlantingSettings& getPlantingSettings();
    const ValleyMaps& getValleyMaps() const;
    // The land as drawn, which everything stands on.
    const TerrainHeights& getGroundHeights() const;
    Entity getSun() const;
    Entity getAir() const;
    PlayerControls& getControls();
    ForestSunSettings& getSunSettings();
    ForestMistSettings& getMistSettings();
    ForestGroundSettings& getGroundSettings();
    ForestRockSettings& getRockSettings();
    ForestStructureSettings& getStructureSettings();
    ForestBastionSettings& getBastionSettings();
    // The towers' light strips (the Bastion's first, then the far towers'), and the Bastion's floodlights.
    const std::vector<Entity>& getLightStrips() const;
    const std::vector<Entity>& getFloodlights() const;
    // The obstruction lights (scene/beacon.h): the Bastion's first (getBastionBeaconCount of them), then the far towers'.
    const std::vector<Entity>& getBeacons() const;
    std::size_t getBastionBeaconCount() const;
    // The player's flashlight (F, off at the start) and its entity.
    FlashlightSettings& getFlashlightSettings();
    Entity getFlashlight() const;
    // Every structure's entity: the Ring, the Bastion and its bridge, the far towers.
    const std::vector<Entity>& getStructures() const;
    const std::vector<Entity>& getRockShapes() const;
    ForestHangingSettings& getHangingSettings();
    // Each island's roots and vines (bark) and then its leaves, island by island.
    const std::vector<Entity>& getIslandHangings() const;
    ForestMossSettings& getMossSettings();
    // The moss under each pillar's ledges and each island, placed as the shape it hangs from.
    const std::vector<Entity>& getMoss() const;
    ForestDriftSettings& getDriftSettings();
    // The rocks drifting round the islands and some pillars.
    const std::vector<Entity>& getDriftingRocks() const;
    // The canopy trees' settings and places, and the emergents' (docs/research/JUNGLE.md).
    ForestTreeSettings& getTreeSettings();
    const std::vector<TreePlacement>& getTreePlacements() const;
    ForestTreeSettings& getEmergentSettings();
    const std::vector<TreePlacement>& getEmergentPlacements() const;
    // The mid layer's (docs/research/JUNGLE.md).
    ForestTreeSettings& getMidSettings();
    const std::vector<TreePlacement>& getMidPlacements() const;
    // Each species' variants as the GPU's cells draw them (no entity a tree): bark, leaves (treeLeavesModel), card, the
    // shadow's stand-ins, then any climbers and epiphytes, then the twigs (buildTwigs); the canopy's, the emergents', the
    // mid layer's.
    const std::vector<PlantedKindLook>& getTreeLooks() const;
    // The understorey's settings and places.
    ForestUnderstoreySettings& getUnderstoreySettings();
    const std::vector<UnderstoreyPlacement>& getUnderstoreyPlacements() const;
    // The palms' settings and places.
    ForestPalmSettings& getPalmSettings();
    const std::vector<PalmPlacement>& getPalmPlacements() const;
    // The understorey's and palms' variants as the GPU's cells draw them (no entity a plant): each its seen mesh, its
    // shadow's stand-in and its card; the understorey's kinds' variants in order, then the palms'.
    const std::vector<PlantedKindLook>& getPlantLooks() const;
    // The lianas' settings and their meshes' entities (bark and ivy, a chunk of ground each).
    ForestLianaSettings& getLianaSettings();
    const std::vector<Entity>& getLianas() const;
    // The climbers' settings and their models in the looks (stems and leaves, of each big variant carrying any).
    ForestClimberSettings& getClimberSettings();
    std::vector<ModelRenderer> getClimberModels() const;
    // The placed trees carrying climbers, in the order forest-climber numbers them.
    const std::vector<TreePlacement>& getClimbedTrees() const;
    const std::vector<Vec3>& getClimbedFacing() const;
    // The epiphytes' settings and their models in the looks (rosettes and moss, of each variant carrying any).
    ForestEpiphyteSettings& getEpiphyteSettings();
    std::vector<ModelRenderer> getEpiphyteModels() const;
    // Each tree carrying rosettes' first rosette in the world, in the order forest-epiphyte numbers them.
    const std::vector<Vec3>& getEpiphyteViews() const;
    // The grass's settings and its cells (baked into its layer's map: the GPU's cells draw a patch each).
    ForestGrassSettings& getGrassSettings();
    const std::vector<GrassCell>& getGrassCells() const;
    // The forest floor's cover's settings and its cells, as the grass's.
    ForestFloorSettings& getFloorSettings();
    // The wind over the valley, on the air entity (scene/wind.h).
    Wind& getWindSettings();
    // How the forest sways in it (games/forest/forest_sway.h), applied live to everything swaying.
    ForestSwaySettings& getSwaySettings();
    const std::vector<GroundCell>& getFloorCells() const;
    // The ground cover's patches as the GPU's cells draw them (no entity a cell): the grass's kinds, then the floor's,
    // each a look of its layer (forestGrassPlanting, forestFloorPlanting), its kind its patch's (forestCoverKind).
    const std::vector<PlantedKindLook>& getCoverLooks() const;
    // The field the GPU's cells plant from (on the air entity): the valley's maps with the ground cover's, every layer.
    const PlantingField& getPlantingField(const World& world) const;
    // The twigs of modelled leaves (games/forest/twigs.h), and the stand forest-twigs shows them on: an entity a
    // prototype's level, prototype by prototype, each hidden until shown.
    ForestTwigSettings& getTwigSettings();
    const std::vector<TwigPrototype>& getTwigPrototypes() const;
    const std::vector<Entity>& getTwigStand() const;
    // Each tree look's twigs (getTreeLooks' order): a twig where each of its leaves' cards is (clusterTwigs).
    const std::vector<std::vector<PlacedTwig>>& getTreeTwigs() const;
    // And as the renderer takes them (scene/twig_set.h): over the prototypes' meshes, each with its levels given.
    const std::vector<std::shared_ptr<const TwigSet>>& getTreeTwigSets() const;

private:
    // Point the sun and set the air (sky and mist) for the current settings, and the ground's blend.
    void updateDaylight(World& world);

    // Give every swaying thing its sway when the sway's settings changed, announcing the still draws changed.
    void applySway(World& world);

    // The land's rings, each a static ground entity only where it is walked on (the finest).
    void buildGround(World& world, AssetManager& assets);

    // The pillars, islands, the Ring, the Monolith and the far towers.
    void buildShapes(World& world, AssetManager& assets);

    // The trees between the Ring and the clearing: the emergents placed first, the canopy trees round them, the mid
    // layer under and between them, each species a few variants grown, each placed many times.
    void buildPlantingMaps();

    // The species' settings every layer is planted from (forest_planting.h).
    ForestSpecies plantingSpecies() const;
    void buildTrees(AssetManager& assets);

    // What a species' variants grow climbers with (games/forest/climbers.h): the settings and the stems' and leaves'
    // materials, made once for every species carrying them.
    struct TreeClimbers {
        const ForestClimberSettings* settings = nullptr;
        Material stem;
        Material leaf;
    };

    // What a species' variants grow epiphytes with (games/forest/epiphytes.h): the settings, whether its trees are big
    // (ferns and many rosettes) and the rosettes' and moss's materials.
    struct TreeEpiphytes {
        const ForestEpiphyteSettings* settings = nullptr;
        bool bigTree = true;
        Material leaf;
        Material moss;
    };

    // One species' variants grown into the looks its placed trees are drawn with, with its leaf material; with
    // climbers, each variant's grown too (none for the mid layer), and likewise with epiphytes.
    // Returns each variant's height (its box's top, in its mesh's units), which its sway bends against.
    std::vector<float> plantTrees(AssetManager& assets, ForestLayer layer, const ForestTreeSettings& settings,
                                  const std::vector<TreePlacement>& placements, std::uint32_t variantSeed, const Material& leaf, const TreeClimbers* climbers, const TreeEpiphytes& epiphytes);

    // The understorey under the trees: each kind's scanned plants (or a stand-in) stood on their feet and simplified, a
    // look each for the GPU's cells.
    void buildUnderstorey(AssetManager& assets);

    // The understorey palms (docs/research/JUNGLE.md, Palms): a few variants grown on every core, a look each.
    void buildPalms(AssetManager& assets);

    // What the GPU's cells draw: the trees' looks, then the plants', over the patch; the ground cover's wherever planted.
    std::shared_ptr<const PlantingLooks> plantedLooks() const;

    // What the GPU's cells plant: the valley's maps and the forest's layers, the ground cover's maps and layers after.
    PlantingField plantingField() const;

    // The grass in the open (docs/research/GROUNDCOVER.md): a few patches grown on every core, a look each, the GPU's
    // cells drawing one a cell of land its map names.
    void buildGrass(AssetManager& assets);

    // The forest floor's cover under the canopy (docs/research/GROUNDCOVER.md): fallen leaves, seedlings and small ferns,
    // as the grass is, on the leaf litter's land.
    void buildFloorCover(AssetManager& assets);

    // The lianas hanging from the canopy and emergent trees' crowns (docs/research/VINES.md, stage 1), a chunk of ground a mesh.
    void buildLianas(AssetManager& assets);
    void updateNearLianas(World& world, int most);

    // Every tree's trunk as far round as the floor's plants keep from it: the big trees' buttresses, the mid layer's trunk.

    // Place the camera at a named view; false for a name there is none of.
    bool showView(World& world, const std::string& name);

    // The twig prototypes, from the canopy's leaves, and their stand's entities, hidden (ROADMAP phase 21 stage 3a).
    void buildTwigs(World& world, AssetManager& assets);

    // The stand shown in front of the camera (a row a prototype, its levels left to right, facing the camera), or hidden.
    void showTwigStand(World& world, bool shown);

    std::string assetFolder;
    PlayerControls controls;
    ForestSunSettings sunSettings;
    ForestMistSettings mistSettings;
    ForestGroundSettings groundSettings;
    ForestRockSettings rockSettings;
    ForestStructureSettings structureSettings;
    ForestBastionSettings bastionSettings;

    // What the ground's and the still rocks' materials were last given (updateDaylight): they are written, and their
    // still draws built again, only when the settings differ.
    std::optional<ForestGroundSettings> appliedGroundSettings;
    std::optional<ForestRockSettings> appliedRockSettings;
    std::optional<ForestStructureSettings> appliedStructureSettings;
    std::optional<ForestBastionSettings> appliedBastionSettings;
    ForestHangingSettings hangingSettings;
    ForestMossSettings mossSettings;
    ForestDriftSettings driftSettings;
    ForestTreeSettings treeSettings;
    ForestTreeSettings emergentSettings = emergentTreeSettings();
    ForestTreeSettings midSettings = midTreeSettings();
    ForestUnderstoreySettings understoreySettings;
    ForestPalmSettings palmSettings;
    ForestLianaSettings lianaSettings;
    ForestNearTreeSettings nearTreeSettings;
    ForestClimberSettings climberSettings;
    ForestEpiphyteSettings epiphyteSettings;
    ForestGrassSettings grassSettings;
    ForestTwigSettings twigSettings;
    std::vector<TwigPrototype> twigPrototypes;
    std::vector<Entity> twigStand;
    std::vector<std::vector<PlacedTwig>> treeTwigs; // each tree look's, as treeLooks
    std::vector<std::shared_ptr<const TwigSet>> treeTwigSets; // and as the renderer takes them (buildTwigs)

    // The wind over the valley (docs/research/WIND.md): down it, along the path from the rim towards the Monolith (-z), a few m/s
    // above the canopy as measured over rainforests; under the canopy the plants' springs answer it less.
    Wind windSettings{.towardsDegrees = 270.0f, .speed = 3.0f};

    // How each kind of growth sways in it, what its swaying things were last given (applySway), and those things.
    ForestSwaySettings swaySettings;
    std::optional<ForestSwaySettings> appliedSwaySettings;
    std::vector<ForestSwayer> swayers;

    // The canopy and emergent trees' variants' heights (plantTrees), and the anchors the lianas hang from in their crowns
    // (scene/wind.h), on the air entity, their bend the trees' (applySway).
    std::vector<float> canopyHeights;
    std::vector<float> emergentHeights;
    ForestFloorSettings floorSettings;
    ForestLeafLightSettings leafLightSettings;
    ForestValleySettings valleySettings;
    ForestPlantingSettings plantingSettings;
    ValleyMaps valleyMaps;
    std::shared_ptr<const TerrainHeights> groundHeights; // the land drawn, which the plants stand on

    // The real sky's air, on the air entity, with the mist's Fog while it is on.
    Atmosphere atmosphereSettings;

    Entity ground;   // the finest ring, walked on and hidden
    Entity terrain;  // the land drawn (scene/terrain.h), a region a ring
    std::vector<float> groundTexels; // each ring's paint texels in metres, the finest first
    std::vector<PillarPlacement> pillars;
    std::vector<IslandPlacement> islands;
    std::vector<Entity> rockShapes; // every pillar and island, which share the rock's material
    std::vector<Entity> hangingEntities;
    std::vector<Entity> mossEntities;
    std::vector<Entity> driftingRocks;
    std::vector<TreePlacement> treePlacements;
    std::vector<TreePlacement> emergentPlacements;
    std::vector<TreePlacement> midPlacements;
    std::vector<UnderstoreyPlacement> understoreyPlacements;
    std::vector<PalmPlacement> palmPlacements;
    NearLianas nearLianas;
    NearLianaLook lianaLook;
    std::vector<TreePlacement> climbedTrees;
    std::vector<Vec3> climbedFacing; // each one's first climber's way out of the trunk, in the world (forest-climber looks from it)
    std::vector<PlantedKindLook> treeLooks; // each species' variants as the GPU's cells draw the trees (plantTrees)
    std::vector<PlantedKindLook> plantLooks; // the understorey's and palms' variants as the GPU's cells draw them

    // A look's model swaying as a tree (applySway gives it its sway): its look, its model, how it sways.
    struct LookSwayer {
        std::size_t look = 0;
        std::size_t model = 0;
        ForestSwayer swayer;
    };

    std::vector<LookSwayer> treeLookSwayers;
    std::vector<LookSwayer> plantLookSwayers;
    std::vector<PlantedKindLook> coverLooks; // the grass's and the floor's patches as the GPU's cells draw them
    std::vector<LookSwayer> coverLookSwayers;

    // A model of a look: the climbers' and the epiphytes' (getClimberModels, getEpiphyteModels).
    struct LookModel {
        std::size_t look = 0;
        std::size_t model = 0;
    };

    std::vector<LookModel> climberModels;
    std::vector<LookModel> epiphyteModels;
    std::vector<ModelRenderer> lookModels(const std::vector<LookModel>& listed) const;
    std::vector<GrassCell> grassCells;
    std::vector<GroundCell> floorCells;
    PlantingCoverMap grassMap; // the cells baked for the GPU's cells (forestCoverMap)
    PlantingCoverMap floorMap;
    float grassRelief = 0.0f; // the land's relief under the cells (forestCoverRelief)
    float floorRelief = 0.0f;
    std::vector<Vec3> epiphyteViews;   // each tree carrying rosettes' first rosette, in the world
    std::vector<Vec3> epiphyteOutward; // the level way from its trunk to it (forest-epiphyte looks from there)
    Entity ring;
    std::vector<Entity> monolith; // the Bastion, then its bridge
    std::vector<Entity> structures;
    std::vector<Entity> lightStrips;
    std::vector<Entity> floodlights;
    std::vector<Entity> beacons;
    std::size_t bastionBeaconCount = 0;
    // The last tick's end on the simulation clock and a tick's length, so each frame's flashes are timed between ticks.
    double beaconClock = 0.0;
    float beaconTick = 0.0f;
    Flashlight flashlight;
    FlashlightSettings flashlightSettings;
    Entity sun;
    Entity air;
    Entity floorMist; // the mist's pool on the valley's floor, a second Fog beside the air's haze
    Entity probes;

    // What is walked on: the finest ring of ground and the Monolith, built once in onInit, never rebuilt.
    StaticLevel staticLevel;

    // F4: the engine's bounding boxes and culling readout.
    bool debugViewEnabled = false;
};
