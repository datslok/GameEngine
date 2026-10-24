#pragma once

#include "cloth/cloth.h"
#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "engine/game.h"
#include "gameplay/control_mode.h"
#include "gameplay/player_controls.h"
#include "physics/static_level.h"

#include <vector>

/*
* The afternoon the lab is lit by: a high summer sun at 45 degrees north, from the side so folds cast shadows, under
* the real sky (an Atmosphere). The sun's light is above the air, as the light lab's.
*/
inline constexpr float clothLabLatitudeDegrees = 45.0f;
inline constexpr float clothLabDayOfYear = 172.0f;
inline constexpr float clothLabSolarHours = 15.0f;
inline constexpr float clothLabSunIntensity = 4.0f;

// The left button grabs the sheet within this reach of the eye, so it cannot be pulled from across the field.
inline constexpr float clothLabGrabReach = 10.0f;

/*
* The cloth lab: a test ground for phase 7's cloth, a station for each milestone on a plain floor under an afternoon
* sun (games/clothlab/cloth_lab_layout.h). The same controls as the other games (F1 first person, F2 MOBA view, F3
* free camera, F4 the debug view), and the left button held drags the sheet: through the crosshair while the mouse
* looks, through the cursor in the MOBA view.
*/
class ClothLabGame final : public Game {
public:
    explicit ClothLabGame(ControlMode startMode = ControlMode::FreeCamera, bool debugModeSwitching = true);

    const char* title() const override;
    void onInit(World& world, AssetManager& assets) override;
    void onInput(World& world, const Input& input) override;
    void onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) override;
    void onUpdate(World& world, const Input& input, float frameSeconds, float alpha) override;
    Camera& getCamera() override;
    bool wantsMouseLook() const override;
    Vec3 getLightFocus() override;
    bool wantsDebugView() const override;
    void onDebugDraw(const World& world, DebugDraw& debug, float alpha) override;
    void visitSettings(SettingsVisitor& visitor) override;
    std::vector<GoldenView> goldenViews() const override;
    void prepareGoldenView(World& world, const GoldenView& view) override;

    // For tests.
    const StaticLevel& getLevel() const;

    // The hanging sheet: a Cloth, the DeformingMesh it drives and a ModelRenderer drawing it.
    Entity getSheet() const;
    ClothSettings& getClothSettings();
    PlayerControls& getControls();

    // The hand holding the sheet while the left button is held: the command the next ticks carry out.
    const std::optional<ClothGrab>& getSheetGrab() const;

private:
    // Turn the left button into the hand's command: grab the sheet under the pointer, move the hand along it, let go.
    void updateGrab(const World& world, const Input& input);

    PlayerControls controls;

    // The hanging sheet, and its fabric (the Cloth group in F5), given to it every tick.
    Entity sheet;
    ClothSettings clothSettings;

    // The hand, and how far from the eye the sheet was grabbed: the hand stays that far along the pointer.
    std::optional<ClothGrab> sheetGrab;
    float grabDistance = 0.0f;

    // The floor, the stations' blocks and the ball: built once in onInit, never rebuilt.
    StaticLevel staticLevel;

    // F4: the engine's bounding boxes and culling readout.
    bool debugViewEnabled = false;
};
