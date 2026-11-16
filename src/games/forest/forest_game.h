#pragma once

#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "engine/game.h"
#include "games/forest/drifting_rocks.h"
#include "games/forest/forest_ground.h"
#include "games/forest/forest_trees.h"
#include "games/forest/hanging_moss.h"
#include "games/forest/island_hangings.h"
#include "games/forest/climbers.h"
#include "games/forest/epiphytes.h"
#include "games/forest/floor_cover.h"
#include "games/forest/grass.h"
#include "games/forest/lianas.h"
#include "games/forest/palms.h"
#include "games/forest/understorey.h"
#include "games/forest/valley_shapes.h"
#include "gameplay/control_mode.h"
#include "gameplay/player_controls.h"
#include "physics/static_level.h"
#include "scene/atmosphere.h"
#include "scene/fog.h"
#include "scene/wind.h"
#include "games/forest/forest_sway.h"

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
* The valley's haze (a Fog, render/gpu/volumetric_fog.h): humid air filling the valley, about 6 km of sight on its floor
* (3 / density), thinning by e every 300 m up, so the pillars' tops and the islands stand clearer than their feet, out
* to 20 km along a ray (the far plane). A first guess for the rough valley, to be judged by eye.
*/
struct ForestMistSettings {
    bool enabled = true;
    Fog fog{.density = 0.0005f, .baseHeight = 0.0f, .scaleHeight = 300.0f, .maxDistance = 20000.0f};
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
    Entity getSun() const;
    Entity getAir() const;
    PlayerControls& getControls();
    ForestSunSettings& getSunSettings();
    ForestMistSettings& getMistSettings();
    ForestGroundSettings& getGroundSettings();
    ForestRockSettings& getRockSettings();
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
    // The canopy trees' settings and places, and the emergents' (docs/JUNGLE.md).
    ForestTreeSettings& getTreeSettings();
    const std::vector<TreePlacement>& getTreePlacements() const;
    ForestTreeSettings& getEmergentSettings();
    const std::vector<TreePlacement>& getEmergentPlacements() const;
    // The mid layer's (docs/JUNGLE.md).
    ForestTreeSettings& getMidSettings();
    const std::vector<TreePlacement>& getMidPlacements() const;
    // Each tree's bark, its leaves and its shadow leaves, tree by tree: the canopy trees, the emergents, the mid layer.
    const std::vector<Entity>& getTrees() const;
    const std::vector<Entity>& getTreeImpostors() const;
    // The understorey's settings and places, and each plant's leaves and then its shadow's stand-in, plant by plant.
    ForestUnderstoreySettings& getUnderstoreySettings();
    const std::vector<UnderstoreyPlacement>& getUnderstoreyPlacements() const;
    const std::vector<Entity>& getUnderstorey() const;
    // Each plant's card (scene/impostor.h), plant by plant.
    const std::vector<Entity>& getUnderstoreyImpostors() const;
    // The palms' settings and places, each palm's seen mesh and then its shadow's stand-in, palm by palm, and its card.
    ForestPalmSettings& getPalmSettings();
    const std::vector<PalmPlacement>& getPalmPlacements() const;
    const std::vector<Entity>& getPalms() const;
    const std::vector<Entity>& getPalmImpostors() const;
    // The lianas' settings and their meshes' entities (bark and ivy, a chunk of ground each).
    ForestLianaSettings& getLianaSettings();
    const std::vector<Entity>& getLianas() const;
    // The climbers' settings and their entities (stems and leaves, on each placed big tree whose variant carries any).
    ForestClimberSettings& getClimberSettings();
    const std::vector<Entity>& getClimbers() const;
    // The placed trees carrying climbers, in the order forest-climber numbers them.
    const std::vector<TreePlacement>& getClimbedTrees() const;
    const std::vector<Vec3>& getClimbedFacing() const;
    // The epiphytes' settings and their entities (rosettes and moss, on each placed tree whose variant carries any).
    ForestEpiphyteSettings& getEpiphyteSettings();
    const std::vector<Entity>& getEpiphytes() const;
    // Each tree carrying rosettes' first rosette in the world, in the order forest-epiphyte numbers them.
    const std::vector<Vec3>& getEpiphyteViews() const;
    // The grass's settings, its cells and their entities (a patch each, following the ground), in the cells' order.
    ForestGrassSettings& getGrassSettings();
    const std::vector<GrassCell>& getGrassCells() const;
    const std::vector<Entity>& getGrass() const;
    // The forest floor's cover's settings, its cells and their entities (a patch each, following the ground).
    ForestFloorSettings& getFloorSettings();
    // The wind over the valley, on the air entity (scene/wind.h).
    Wind& getWindSettings();
    // How the forest sways in it (games/forest/forest_sway.h), applied live to everything swaying.
    ForestSwaySettings& getSwaySettings();
    const std::vector<GroundCell>& getFloorCells() const;
    const std::vector<Entity>& getFloorCover() const;

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
    void buildTrees(World& world, AssetManager& assets);

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

    // One species' variants grown and its placed trees added, bark then leaves, with its leaf material; with climbers,
    // each variant's grown too (none for the mid layer), and likewise with epiphytes.
    void plantTrees(World& world, AssetManager& assets, const ForestTreeSettings& settings, const std::vector<TreePlacement>& placements,
                    std::uint32_t variantSeed, const Material& leaf, const TreeClimbers* climbers, const TreeEpiphytes& epiphytes);

    // The understorey under the trees: each kind's scanned plants (or a stand-in) stood on their feet and simplified,
    // placed many times.
    void buildUnderstorey(World& world, AssetManager& assets);

    // The understorey palms (docs/JUNGLE.md, Palms): a few variants grown on every core, placed many times.
    void buildPalms(World& world, AssetManager& assets);

    // The grass in the open (docs/GROUNDCOVER.md): a few patches grown on every core, a cell of land each.
    void buildGrass(World& world, AssetManager& assets);

    // The forest floor's cover under the canopy (docs/GROUNDCOVER.md): fallen leaves, seedlings and small ferns, as the
    // grass is, a cell of the leaf litter's land each.
    void buildFloorCover(World& world, AssetManager& assets);

    // The lianas hanging from the canopy and emergent trees' crowns (docs/VINES.md, stage 1), a chunk of ground a mesh.
    void buildLianas(World& world, AssetManager& assets);

    // Every tree's trunk as far round as the floor's plants keep from it: the big trees' buttresses, the mid layer's trunk.
    std::vector<StandingCrown> floorTrunks() const;

    // Place the camera at a named view; false for a name there is none of.
    bool showView(World& world, const std::string& name);

    std::string assetFolder;
    PlayerControls controls;
    ForestSunSettings sunSettings;
    ForestMistSettings mistSettings;
    ForestGroundSettings groundSettings;
    ForestRockSettings rockSettings;

    // What the ground's and the still rocks' materials were last given (updateDaylight): they are written, and their
    // still draws built again, only when the settings differ.
    std::optional<ForestGroundSettings> appliedGroundSettings;
    std::optional<ForestRockSettings> appliedRockSettings;
    ForestHangingSettings hangingSettings;
    ForestMossSettings mossSettings;
    ForestDriftSettings driftSettings;
    ForestTreeSettings treeSettings;
    ForestTreeSettings emergentSettings = emergentTreeSettings();
    ForestTreeSettings midSettings = midTreeSettings();
    ForestUnderstoreySettings understoreySettings;
    ForestPalmSettings palmSettings;
    ForestLianaSettings lianaSettings;
    ForestClimberSettings climberSettings;
    ForestEpiphyteSettings epiphyteSettings;
    ForestGrassSettings grassSettings;

    // The wind over the valley (docs/WIND.md): down it, along the path from the rim towards the Monolith (-z), a few m/s
    // above the canopy as measured over rainforests; under the canopy the plants' springs answer it less.
    Wind windSettings{.towardsDegrees = 270.0f, .speed = 3.0f};

    // How each kind of growth sways in it, what its swaying things were last given (applySway), and those things.
    ForestSwaySettings swaySettings;
    std::optional<ForestSwaySettings> appliedSwaySettings;
    std::vector<ForestSwayer> swayers;
    ForestFloorSettings floorSettings;
    ForestLeafLightSettings leafLightSettings;
    ForestValleySettings valleySettings;

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
    std::vector<Entity> understorey;
    std::vector<Entity> understoreyImpostors; // a card each plant, in the placements' order (scene/impostor.h)
    std::vector<PalmPlacement> palmPlacements;
    std::vector<Entity> palms;
    std::vector<Entity> palmImpostors; // a card each palm, in the placements' order
    std::vector<Entity> lianaEntities;
    std::vector<Entity> climberEntities;
    std::vector<TreePlacement> climbedTrees;
    std::vector<Vec3> climbedFacing; // each one's first climber's way out of the trunk, in the world (forest-climber looks from it)
    std::vector<Entity> epiphyteEntities;
    std::vector<GrassCell> grassCells;
    std::vector<Entity> grassEntities;
    std::vector<GroundCell> floorCells;
    std::vector<Entity> floorEntities;
    std::vector<Vec3> epiphyteViews;   // each tree carrying rosettes' first rosette, in the world
    std::vector<Vec3> epiphyteOutward; // the level way from its trunk to it (forest-epiphyte looks from there)
    std::vector<Entity> trees;
    std::vector<Entity> treeImpostors; // a card each tree, in the trees' order (scene/impostor.h)
    Entity ring;
    std::vector<Entity> monolith;
    Entity sun;
    Entity air;
    Entity probes;

    // What is walked on: the finest ring of ground and the Monolith, built once in onInit, never rebuilt.
    StaticLevel staticLevel;

    // F4: the engine's bounding boxes and culling readout.
    bool debugViewEnabled = false;
};
