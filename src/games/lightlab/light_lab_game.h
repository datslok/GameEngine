#pragma once

#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "engine/game.h"
#include "gameplay/control_mode.h"
#include "gameplay/flashlight.h"
#include "gameplay/player_controls.h"
#include "physics/static_level.h"
#include "scene/light.h"

#include <vector>

/*
* Where and when the light lab's sun shines (scene/sun) and how bright the day is. The solar time is the clock: it moves
* on by itself at hoursPerSecond (0.1 by default; golden views hold it still), so its slider shows the time of day as
* it passes.
* The brightness values are picked by eye for daylight through the default tone mapping, since lights have no physical
* units yet. The sky is uniform until a real sky (phase 6d) replaces it; the light probes see it and carry its light
* round the level, and the flat ambient light (where there are no probes) is the light that same sky gives.
*/
struct LightLabSunSettings {
    float latitudeDegrees = 45.0f;
    float dayOfYear = 80.0f;       // the March equinox: sunrise near 6, sunset near 18
    float solarHours = 15.5f;      // mid-afternoon: the sun low in the west-south-west, long shadows, the window lit
    float hoursPerSecond = 0.1f;   // a whole day in 4 minutes
    float sunIntensity = 3.0f;
    float skyBrightness = 1.0f;    // the sky at full daylight, seen behind everything and lighting it; it dims towards dusk

    // The sun disc in the sky against its real size (0.53 degrees across), for looks only: shadows keep the real size,
    // and a bigger disc glows dimmer by as much, so it gives the same light.
    float sunDiscScale = 1.0f;
};

void visit(LightLabSunSettings& settings, SettingsVisitor& visitor);

// The light probes' grid: the usual one round the building, 1 m apart, or a test of finer bounce light inside it
// (labDenseProbeVolume, 20 cm apart, about 20 times the probe work). Read every frame; changing it rebuilds the probes.
struct LightLabProbeSettings {
    bool denseProbes = false;
};

void visit(LightLabProbeSettings& settings, SettingsVisitor& visitor);

// The solar time after some seconds pass at the given speed, kept within one day (0 to 24 hours).
float advanceSolarHours(float solarHours, float hoursPerSecond, float seconds);

/*
* The day's light for the sun in a direction: sunlight travelling away from it, fading out as the sun sinks below the
* horizon and warmer the lower it stands (a stand-in for the atmosphere's reddening), and the sky seen behind everything
* with the ambient light it gives (the same value: a uniform sky drawn at B gives a surface facing it the irradiance B),
* blue, dimming towards dusk but never quite black.
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
    LightLabProbeSettings& getProbeSettings();
    Entity getSun() const;

    // The glowing disc in the sky where the sunlight comes from (scene/sky_object.h).
    Entity getSunDisc() const;
    Entity getFirstPersonBody() const;
    Entity getFlashlight() const;
    PlayerControls& getControls();

private:
    // Point the sun, set the sky and place the sun disc for the current settings and camera.
    void updateDaylight(World& world);

    PlayerControls controls;
    LightLabSunSettings sunSettings;
    LightLabProbeSettings probeSettings;

    // The entity holding the frame's ProbeVolume, set from probeSettings every frame.
    Entity probes;

    Entity sun;
    Entity sunDisc;
    Entity sky;

    // The ground and every block: built once in onInit, never rebuilt.
    StaticLevel staticLevel;

    // F toggles it, held in the right hand (off at the start, so the golden views keep the sunlit picture): for seeing
    // how a moving light's bounce follows it.
    Flashlight flashlight;

    // F4: the engine's bounding boxes and culling readout.
    bool debugViewEnabled = false;
};
