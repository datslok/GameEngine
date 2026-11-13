#pragma once

#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "engine/game.h"
#include "games/forest/forest_ground.h"
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
* The valley mist (a Fog, render/gpu/volumetric_fog.h): humid air pooling in the hollows below the clearing, about
* 75 m of sight at its base 2 m below the clearing, thinning by e every 4 m up, picked by eye.
*/
struct ForestMistSettings {
    bool enabled = true;
    Fog fog{.density = 0.04f, .baseHeight = -2.0f, .scaleHeight = 4.0f};
};

void visit(ForestMistSettings& settings, SettingsVisitor& visitor);

// The ground's blend between its layers (Material::groundBlendDepth): shallow keeps borders crisp along the textures'
// own shapes, deep cross-fades them.
struct ForestGroundSettings {
    float blendDepth = 0.2f;
};

void visit(ForestGroundSettings& settings, SettingsVisitor& visitor);

/*
* The forest showcase (ROADMAP phase 18): a lush, damp forest with mist in its valleys, grass where the canopy opens and
* a mechanical structure in a clearing. So far the land: rolling hills with a clearing and a path south to a meadow
* (forest_terrain.h), its ground blended from scanned grass, leaf litter, mud and mossy rock (forest_ground.h; plain
* stand-ins until tools/fetch_forest_assets.py has fetched them), a sun near the equator through a real sky and the
* valley mist. The same controls as the other games (F1 first person, F2 MOBA view, F3 free camera).
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
    std::vector<GoldenView> goldenViews() const override;
    void prepareGoldenView(World& world, const GoldenView& view) override;

    // For tests.
    const StaticLevel& getLevel() const;
    Entity getGround() const;
    Entity getSun() const;
    Entity getAir() const;
    PlayerControls& getControls();
    ForestSunSettings& getSunSettings();
    ForestMistSettings& getMistSettings();
    ForestGroundSettings& getGroundSettings();

private:
    // Point the sun and set the air (sky and mist) for the current settings, and the ground's blend.
    void updateDaylight(World& world);

    std::string assetFolder;
    PlayerControls controls;
    ForestSunSettings sunSettings;
    ForestMistSettings mistSettings;
    ForestGroundSettings groundSettings;

    // The real sky's air, on the air entity, with the mist's Fog while it is on.
    Atmosphere atmosphereSettings;

    Entity ground;
    Entity sun;
    Entity air;
    Entity probes;

    // The ground: built once in onInit, never rebuilt.
    StaticLevel staticLevel;

    // F4: the engine's bounding boxes and culling readout.
    bool debugViewEnabled = false;
};
