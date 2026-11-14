#pragma once

#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "engine/game.h"
#include "games/forest/forest_ground.h"
#include "games/forest/valley_shapes.h"
#include "gameplay/control_mode.h"
#include "gameplay/player_controls.h"
#include "physics/static_level.h"
#include "scene/atmosphere.h"
#include "scene/fog.h"

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
// own shapes, deep cross-fades them.
struct ForestGroundSettings {
    float blendDepth = 0.2f;
};

void visit(ForestGroundSettings& settings, SettingsVisitor& visitor);

/*
* The pillars' and islands' rock (Material's projected strata): the blend between beds (shallow keeps their edges
* following the rocks' own shapes, deep cross-fades them), where the moss starts and is full, as the upward part of
* the surface's normal (1 level, 0 a wall), how far the beds rise and fall either way, and how dark the water streaks
* down the sides are (0 none, 1 black).
*/
struct ForestRockSettings {
    float blendDepth = 0.3f;
    float mossStart = 0.55f;
    float mossFull = 0.8f;
    float warpMetres = 6.0f;
    float streakStrength = 0.45f;
};

void visit(ForestRockSettings& settings, SettingsVisitor& visitor);

/*
* What the valley is built with, read once at start-up: the finest ground squares (2 m, or 4 or 8 m for a quicker start:
* 2 << groundCoarseness), and how many pillars and floating islands there are (fewer if they run out of room).
*/
struct ForestValleySettings {
    int groundCoarseness = 0;
    int pillarCount = 220;
    int islandCount = 30;
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
* and far towers at their real sizes (valley_shapes.h), a sun near the equator through a real sky and the valley's haze.
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
    void visitSettings(SettingsVisitor& visitor) override;
    void addCommands(CommandRegistry& registry, World& world) override;
    std::vector<GoldenView> goldenViews() const override;
    void prepareGoldenView(World& world, const GoldenView& view) override;

    // For tests.
    const StaticLevel& getLevel() const;
    Entity getGround() const;
    const std::vector<Entity>& getFarGround() const;
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

private:
    // Point the sun and set the air (sky and mist) for the current settings, and the ground's blend.
    void updateDaylight(World& world);

    // The land's rings, each a static ground entity only where it is walked on (the finest).
    void buildGround(World& world, AssetManager& assets);

    // The pillars, islands, the Ring, the Monolith and the far towers.
    void buildShapes(World& world, AssetManager& assets);

    // Place the camera at a named view; false for a name there is none of.
    bool showView(World& world, const std::string& name);

    std::string assetFolder;
    PlayerControls controls;
    ForestSunSettings sunSettings;
    ForestMistSettings mistSettings;
    ForestGroundSettings groundSettings;
    ForestRockSettings rockSettings;
    ForestValleySettings valleySettings;

    // The real sky's air, on the air entity, with the mist's Fog while it is on.
    Atmosphere atmosphereSettings;

    Entity ground;
    std::vector<Entity> farGround;
    std::vector<float> groundTexels; // each ring's paint texels in metres, the finest first
    std::vector<PillarPlacement> pillars;
    std::vector<IslandPlacement> islands;
    std::vector<Entity> rockShapes; // every pillar and island, which share the rock's material
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
