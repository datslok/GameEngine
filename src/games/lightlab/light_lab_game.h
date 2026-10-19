#pragma once

#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "engine/game.h"
#include "gameplay/control_mode.h"
#include "gameplay/player_controls.h"
#include "physics/static_level.h"
#include "scene/light.h"

#include <vector>

/*
* Where and when the light lab's sun shines (scene/sun) and how bright the day is. The solar time is the clock: it moves
* on by itself at hoursPerSecond (0, still, by default), so its slider shows the time of day as it passes.
* The brightness values are picked by eye for daylight through the default tone mapping, since lights have no physical
* units yet; the sky is a flat ambient stand-in until a real sky (phase 6d) and bounce light (DDGI) replace it.
*/
struct LightLabSunSettings {
    float latitudeDegrees = 45.0f;
    float dayOfYear = 80.0f;       // the March equinox: sunrise near 6, sunset near 18
    float solarHours = 15.5f;      // mid-afternoon: the sun low in the west-south-west, long shadows, the window lit
    float hoursPerSecond = 0.0f;   // 0.1 runs a whole day in 4 minutes
    float sunIntensity = 3.0f;
    float skyAmbient = 0.2f;       // at full daylight; it dims with the sun's height towards dusk
    float skyBrightness = 1.0f;    // the sky seen behind everything, at full daylight, dimming with the ambient
};

void visit(LightLabSunSettings& settings, SettingsVisitor& visitor);

// The solar time after some seconds pass at the given speed, kept within one day (0 to 24 hours).
float advanceSolarHours(float solarHours, float hoursPerSecond, float seconds);

/*
* The day's light for the sun in a direction: sunlight travelling away from it, fading out as the sun sinks below the
* horizon and warmer the lower it stands (a stand-in for the atmosphere's reddening), and the sky's ambient light and
* the sky seen behind everything, both blue, dimming towards dusk but never quite black.
*/
struct Daylight {
    DirectionalLight sun;
    AmbientLight sky;
    SkyBackground background;
};
Daylight daylightFor(const Vec3& towardsSun, const LightLabSunSettings& settings);

/*
* The light lab: a sunlit test ground for shadows and, later, global illumination. A flat field 800 m across with towers
* at doubling distances, a thin fence running north to the far distance, a forest, and a two-room building lit through
* one window by a sun that moves with the time of day. The same controls as the demo (F1 first person, F2 MOBA view, F3
* free camera).
*/
class LightLabGame final : public Game {
public:
    explicit LightLabGame(ControlMode startMode = ControlMode::FreeCamera, bool debugModeSwitching = true);

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
    LightLabSunSettings& getSunSettings();
    Entity getSun() const;
    Entity getFirstPersonBody() const;
    PlayerControls& getControls();

private:
    // Point the sun and set the sky for the current settings.
    void updateDaylight(World& world);

    PlayerControls controls;
    LightLabSunSettings sunSettings;

    Entity sun;
    Entity sky;

    // The ground and every block: built once in onInit, never rebuilt.
    StaticLevel staticLevel;

    // F4: the engine's bounding boxes and culling readout.
    bool debugViewEnabled = false;
};
