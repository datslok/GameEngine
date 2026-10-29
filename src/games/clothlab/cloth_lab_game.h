#pragma once

#include "cloth/cloth.h"
#include "core/settings_visitor.h"
#include "games/clothlab/cloth_lab_dress.h"
#include "games/clothlab/cloth_lab_layout.h"
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
* The sheet hangs from two hooks on the fabric bar, like a curtain on a rail. Hook gap (Hooks in F5) is how far apart
* they should be, at most the sheet's width; they slide there at this speed, about the sheet's middle, so bunching the
* sheet up shows how its bending makes folds.
*/
struct ClothLabHookSettings {
    float hookGap = clothLabSheetWidth; // m
};

void visit(ClothLabHookSettings& settings, SettingsVisitor& visitor);

/*
* Which stations have cloth, read when the lab starts. The big sheet (10,000 points) is the solver's heaviest work: about
* 6.3 ms a tick on 16 threads with isometric bending, inside a tick's 8.3 (with the hinge model about 11, so the lab
* runs a little slower than real time). Off by
* default; on for measuring the solver's threads.
*
* The mannequin (assets/models/human.obj, with a distance field of about 6 million samples built at the start, under a
* second on every core) and the dress sewn round it (cloth_lab_dress.h, about 12,000 points): on by default. The dress
* is pinned at the shoulders and tethered to its straps by default; off, it is held up by contact with the shoulders
* alone, as the dress yardstick also measures it.
*/
struct ClothLabStationSettings {
    bool bigSheet = false;
    bool mannequin = true;
    bool pinnedDress = true;
};

void visit(ClothLabStationSettings& settings, SettingsVisitor& visitor);

inline constexpr float clothLabHookSpeed = 0.5f; // m/s
inline constexpr float clothLabSmallestHookGap = 0.05f;

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
    bool poseCamera(World& world, const Vec3& position, const Vec3& target) override;
    bool wantsMouseLook() const override;
    Vec3 getLightFocus() override;
    bool wantsDebugView() const override;
    void onDebugDraw(const World& world, DebugDraw& debug, float alpha) override;
    void visitSettings(SettingsVisitor& visitor) override;
    std::vector<GoldenView> goldenViews() const override;
    void prepareGoldenView(World& world, const GoldenView& view) override;

    // For tests.
    const StaticLevel& getLevel() const;

    // The hanging sheet on its hooks, and the drape sheet over the ball: each a Cloth, the DeformingMesh it drives and a
    // ModelRenderer drawing it.
    Entity getSheet() const;
    Entity getDrapeSheet() const;

    // The big sheet over the mat, if the stations have it (none otherwise).
    Entity getBigSheet() const;

    // The dress sewn round the mannequin, if the stations have it (none otherwise).
    Entity getDress() const;
    ClothLabStationSettings& getStationSettings();
    ClothSettings& getClothSettings();
    ClothLabHookSettings& getHookSettings();
    PlayerControls& getControls();

    // The hand holding a sheet while the left button is held (the command the next ticks carry out), and which sheet.
    const std::optional<ClothGrab>& getSheetGrab() const;
    Entity getGrabbedSheet() const;

private:
    // Turn the left button into the hand's command: grab the sheet under the pointer, move the hand along it, let go.
    void updateGrab(const World& world, const Input& input);

    PlayerControls controls;

    // The hanging sheet, and its fabric (the Cloth group in F5), given to it every tick.
    Entity sheet;
    ClothSettings clothSettings;

    // Where the hooks should be, and how far apart they are now on their way there.
    ClothLabHookSettings hookSettings;
    float hookGapNow = clothLabSheetWidth;
    Vec3 hookMiddle;

    Entity drapeSheet;
    Entity bigSheet;
    Entity dress;
    ClothLabStationSettings stationSettings;

    // Each of the dress's pins' way onto the shoulders, in the order of its pins (none held by contact alone).
    std::vector<StrapPinPath> strapPaths;

    // Every sheet the lab has.
    std::vector<Entity> sheets() const;

    // The hand, the sheet it holds, and how far from the eye it was grabbed: the hand stays that far along the pointer.
    std::optional<ClothGrab> sheetGrab;
    Entity grabbedSheet;
    float grabDistance = 0.0f;

    // The floor, the stations' blocks and the ball: built once in onInit, never rebuilt.
    StaticLevel staticLevel;

    // F4: the engine's bounding boxes and culling readout.
    bool debugViewEnabled = false;
};
