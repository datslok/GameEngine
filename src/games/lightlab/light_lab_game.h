#pragma once

#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "engine/game.h"
#include "gameplay/control_mode.h"
#include "gameplay/flashlight.h"
#include "gameplay/player_controls.h"
#include "physics/static_level.h"
#include "scene/atmosphere.h"
#include "scene/light.h"

#include <vector>

/*
* Where and when the light lab's sun shines (scene/sun) and how bright the day is. The solar time is the clock: it moves
* on by itself at hoursPerSecond (0, still, by default; golden views hold it still), so its slider shows the time of day as
* it passes.
* With the atmosphere (the default) the renderer draws a real sky lit by the sun, dims and reddens the sunlight on its
* way down through the air, and lights the scene with that sky (render/gpu/atmosphere.h); without it, the lab's own
* hand-made day: a uniform blue sky, a sun warmed by hand towards the horizon, and a disc of its own. The brightness is
* picked by eye through the default tone mapping.
*/
struct LightLabSunSettings {
    float latitudeDegrees = 45.0f;
    int year = 2026;               // for the moon's phase and the stars (scene/celestial.h)
    float dayOfYear = 80.0f;       // the March equinox: sunrise near 6, sunset near 18
    float solarHours = 15.5f;      // mid-afternoon: the sun low in the west-south-west, long shadows, the window lit
    float hoursPerSecond = 0.0f;   // 0.025 runs a whole day in 16 minutes
    // The sun's light: above the air with the atmosphere (in the default mid-afternoon it reaches the ground at about 3,
    // as the hand-made day's did), on the ground without it.
    float sunIntensity = 4.0f;
    float skyBrightness = 1.0f;    // the hand-made sky at full daylight, seen behind everything and lighting it; it dims towards dusk

    // The real sky and air (the lab's Atmosphere settings), or the hand-made day.
    bool atmosphere = true;

    // The sun disc in the sky against its real size (0.53 degrees across), for looks only: shadows keep the real size,
    // and a bigger disc glows dimmer by as much, so it gives the same light.
    float sunDiscScale = 1.0f;
};

void visit(LightLabSunSettings& settings, SettingsVisitor& visitor);

// The solar time after some seconds pass at the given speed, kept within one day (0 to 24 hours).
float advanceSolarHours(float solarHours, float hoursPerSecond, float seconds);

/*
* The clock after some seconds: the solar time moves at hoursPerSecond and carries past midnight into the next day, and
* past the year's last day into the next year, so the moon moves on among the stars through the night.
*/
void advanceLabClock(LightLabSunSettings& settings, float seconds);

/*
* The night sky's dials, picked by eye, not real: the engine has no exposure yet (the real full moon gives a 400,000th of
* the sun's light, which would draw pure black), so the moon and stars are as bright as looks right through the default
* tone mapping.
*/
struct LightLabNightSettings {
    // The full moon's light above the air (the sun's is sunIntensity); the phase dims it (scene/moon.h).
    float moonIntensity = 0.25f;

    // The moon's disc against its real size, for looks only, as the sun's.
    float moonDiscScale = 1.0f;

    // A magnitude 0 star's light, in millionths of the engine's light value (labStarBrightnessUnit), and how far the
    // magnitudes spread the stars' light (1 real, lower squeezes it so faint stars show; scene/stars.h, StarrySky).
    float starBrightness = 1.0f;
    float starContrast = 0.7f;
};

inline constexpr float labStarBrightnessUnit = 1e-6f;

/*
* How much of the stars' light shows for the sun's height: none in daylight, all of it in the dark. Real stars vanish by
* day because the sky outshines them a millionfold, but the dial makes them thousands of times brighter than real, so
* without this they would show through the blue sky. They come out through twilight, the brightest first as the sun
* sinks 4 degrees below the horizon, all of them once it is 14 degrees down (between nautical and astronomical dusk).
*/
inline constexpr float labStarsAppearBelowDegrees = -4.0f;
inline constexpr float labStarsFullBelowDegrees = -14.0f;
float starVisibility(const Vec3& towardsSun);

void visit(LightLabNightSettings& settings, SettingsVisitor& visitor);

// The moonlight's colour: sunlight reflected by the moon's grey-brown ground, a little warmer than the sun's.
inline const Vec3 labMoonColour{1.0f, 0.94f, 0.86f};

/*
* The moon's light from where it stands: from the moon (towardsMoon) at the full moon's intensity dimmed by the phase,
* which the sun's direction sets, at the moon's angular size, shining by the sun's light (reflectsSun, so its disc is
* drawn with its phase). With the atmosphere the renderer dims it on its way down, to nothing below the horizon; without,
* it fades below the horizon here, as the hand-made sun does.
*/
DirectionalLight moonlightFor(const Vec3& towardsMoon, const Vec3& towardsSun, const LightLabNightSettings& settings, bool atmosphere);

// Moving the clock by hand by more than this (the sun about 1.5 degrees) jumps the light: a scene event restarts every
// light probe, which would otherwise take seconds to follow.
inline constexpr float labClockJumpHours = 0.1f;

/*
* The back room's ceiling lamp: a warm bulb (about 2700 K), its reach the room's far corners and a little more, bright
* enough to read by, picked by eye like the sun. A still light, not fast: its bounce goes into the probes' long memory,
* which its switch restarts with a scene event.
*/
inline const PointLight labLampLight{
    .colour = Vec3{1.0f, 0.75f, 0.5f},
    .intensity = 2.0f,
    .range = 7.0f,
    .sourceRadius = 0.5f
};

// The lamp's panel glows this many times its colour while it is on.
inline constexpr float labLampGlow = 6.0f;

/*
* The day's light for the sun in a direction. With the atmosphere, only the sun as it is above the air, white and the
* same at every hour (the renderer does the rest). Without it, the hand-made day: sunlight fading out as the sun sinks
* below the horizon and warmer the lower it stands (a stand-in for the air's reddening), and the sky seen behind
* everything with the ambient light it gives (the same value: a uniform sky drawn at B gives a surface facing it the
* irradiance B), blue, dimming towards dusk but never quite black.
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
    LightLabNightSettings& getNightSettings();
    Entity getSun() const;
    Entity getMoon() const;

    // The glowing disc in the sky where the sunlight comes from (scene/sky_object.h).
    Entity getSunDisc() const;
    Entity getFirstPersonBody() const;
    Entity getFlashlight() const;
    PlayerControls& getControls();

    // The back room's ceiling lamp: its PointLight while on, its fixture's ModelRenderer always.
    Entity getLamp() const;

private:
    // Point the sun, set the sky and place the sun disc for the current settings and camera.
    void updateDaylight(World& world);

    // Switch the ceiling lamp, and tell the probes within its reach.
    void toggleLamp(World& world);

    // A light change for every probe when the clock was moved by hand past labClockJumpHours since it was last seen.
    void signalClockJumps(World& world);

    PlayerControls controls;
    LightLabSunSettings sunSettings;
    LightLabNightSettings nightSettings;

    // The real sky's air (Earth's by default), on the air entity while sunSettings.atmosphere is on.
    Atmosphere atmosphereSettings;

    // The entity holding the lab's ProbeCascades: light probes round the camera wherever it goes.
    Entity probes;

    Entity sun;
    Entity sunDisc;
    Entity sky;
    Entity air;

    // The moon's DirectionalLight, and the StarrySky's entity.
    Entity moon;
    Entity stars;

    // The ground and every block: built once in onInit, never rebuilt.
    StaticLevel staticLevel;

    // F toggles it, held in the right hand (off at the start, so the golden views keep the sunlit picture): for seeing
    // how a moving light's bounce follows it.
    Flashlight flashlight;

    // L toggles it (off at the start): for seeing how a still light's bounce comes and goes.
    Entity lamp;

    // The solar time as the clock itself has moved it since the last jump was signalled.
    float clockHoursSeen = 0.0f;

    // F4: the engine's bounding boxes and culling readout.
    bool debugViewEnabled = false;
};
