#pragma once

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

/*
* The banner's ripples, picked by eye: waves running down it, growing from nothing at its pinned top edge to their full
* height at its foot, and a slower sway of the whole sheet. Only depth into the picture changes, so the sheet stretches a
* little where it bends; real cloth will not.
*/
inline constexpr float clothLabBannerWaveHeight = 0.08f;
inline constexpr float clothLabBannerWavelength = 0.9f;
inline constexpr float clothLabBannerWaveSeconds = 1.6f;
inline constexpr float clothLabBannerSwayHeight = 0.05f;
inline constexpr float clothLabBannerSwaySeconds = 2.3f;

/*
* The cloth lab: a test ground for phase 7's cloth, a station for each milestone on a plain floor under an afternoon
* sun (games/clothlab/cloth_lab_layout.h). The same controls as the other games (F1 first person, F2 MOBA view, F3
* free camera, F4 the debug view).
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
    void visitSettings(SettingsVisitor& visitor) override;
    std::vector<GoldenView> goldenViews() const override;
    void prepareGoldenView(World& world, const GoldenView& view) override;

    // For tests.
    const StaticLevel& getLevel() const;

    // The banner (scene/deforming_mesh.h): a DeformingMesh and a ModelRenderer drawing it.
    Entity getBanner() const;

private:
    PlayerControls controls;

    // The banner and its still shape, which its waves move.
    Entity banner;
    std::vector<Vec3> bannerRest;

    // The floor, the stations' blocks and the ball: built once in onInit, never rebuilt.
    StaticLevel staticLevel;

    // F4: the engine's bounding boxes and culling readout.
    bool debugViewEnabled = false;
};
