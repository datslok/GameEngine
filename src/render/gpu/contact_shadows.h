#pragma once

#include "core/settings_visitor.h"
#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/ambient_occlusion.h"
#include "render/gpu/contact_shadows_shared.h"
#include "render/gpu/upscaling.h"
#include "scene/lighting.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/*
* Contact shadows: short shadows the shadow map misses, from the depth image. A shadow map's texels are millimetres to
* centimetres across and its lookups are pushed towards the light against acne, so a small object's shadow comes loose
* from it or vanishes, worst where the sun grazes. After the depth prepass every pixel marches a short way from its
* surface towards the sun, each step projected onto the screen: a step that crossed the surface the depth image shows
* there since the last step (now behind it by more than a pixel's rounding and by less than a step can reach, the last
* step not) is blocked, and the pixel is in shadow. The depth image shows only fronts, so nothing says how thick a
* thing is; a ray that entered something within the last step can be no further behind its front than one step, so
* anything further behind is something the ray passes behind (a thing nearer the camera, or the far side of a thin one).
* A thin thing's shadow is then too wide by at most a step's reach: more steps, finer shadows. The pass writes a mask the
* main pass multiplies the sun's shadow by; the shadow map stays in charge of everything longer. Only what is on screen
* casts one, so the marches are short and fade at the screen's edge and with distance.
*
* Point and spot lights: the same pass marches towards up to CONTACT_MAX_LIGHTS of them too (localLights), those holding
* a shadow that light the focus most brightly (chooseContactLights), each into a channel of a second mask. Each pixel's
* way to a lamp is its own, and the march stops CONTACT_LIGHT_CLEARANCE short of it, so the lamp's own glowing mesh does
* not shadow it (contactLightRay). The main pass multiplies a light's shadow by its channel, found by its slot in the
* frame's data (contactLightSlotCodes).
*
* The march's start turns with blue noise per pixel and the frame, so anti-aliasing averages the steps.
*/

struct ContactShadowSettings {
    // On by default (off on the Low preset).
    bool enabled = true;

    // How far each pixel marches towards the sun (metres) and in how many steps. A step's reach (length / steps, 0.9 cm)
    // is how much too wide a thin thing's shadow may be, and a thing thinner than it can be stepped over on some frames:
    // at 16 steps (1.9 cm) a 1 cm pen's shadow was half dark; at 32 it is solid, for about twice the pass's time (0.42
    // to 0.77 ms in the user's window on the laptop; chosen by the user by eye on the light lab's pen table).
    // (A fixed 5 cm thickness came before it: a 1 cm pen's shadow was 4 cm too wide; at 0.2 m a fence post cast a
    // second, wider stub of shadow beside its own.)
    float length = 0.3f;
    int steps = 32;

    // The shadow fades out between these view distances (metres), where a pixel covers too much for so short a march.
    float fadeStart = 40.0f;
    float fadeEnd = 60.0f;

    // How many point and spot lights holding a shadow get contact shadows too (0 to CONTACT_MAX_LIGHTS): the ones that
    // light the focus most brightly. Each costs a march for the pixels it reaches.
    int localLights = CONTACT_MAX_LIGHTS;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(ContactShadowSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a length not above 0, steps outside 1..CONTACT_MAX_STEPS, local lights outside
// 0..CONTACT_MAX_LIGHTS, or a fade that ends before it starts (or starts below 0).
void validateContactShadows(const ContactShadowSettings& settings);

// Step number step of steps, turned by noise (0..1): its distance along the march, (step + noise) / steps of length.
float contactStepDistance(int step, int steps, float noise, float length);

// The bias a point must be behind the depth image's surface by to count: CONTACT_BIAS_PIXELS pixels' footprint at
// that view distance (footprint: a pixel's height per metre of view distance, pixelFootprint).
float contactBias(float footprint, float viewDistance);

// How far one step goes: the length over the steps.
float contactStepReach(float length, int steps);

// Whether a step crossed a surface since the last one. behind is how far the step is behind the surface the depth image
// shows at it (along the view; negative in front), previousBehind the last step's (0 for the march's start on its own
// surface, and for a step passed over). Blocked: now behind by more than bias and less than bias + reach, and the last
// step was not.
bool contactBlocked(float previousBehind, float behind, float bias, float reach);

// 0 at the screen's edge, rising to 1 over CONTACT_EDGE_PIXELS inwards (pixel from the top-left corner).
float contactEdgeFade(Vec2 pixel, Vec2 size);

// 1 up to fadeStart, 0 from fadeEnd, linear between.
float contactDistanceFade(float viewDistance, float fadeStart, float fadeEnd);

// The frame's turn of the march's start, CONTACT_NOISE_FRAME_STEP a frame, wrapped into 0..1.
float contactNoiseTurn(std::uint32_t frame);

// What a march needs: the camera-relative view-projection the depths were drawn with (jitter included) and its
// inverse, the render size, a pixel's footprint, the unit way to the light, and the settings.
struct ContactShadowView {
    Mat4 viewProjection;
    Mat4 inverseViewProjection;
    Vec2 size;
    float footprint = 0.0f;
    Vec3 toLight;
    ContactShadowSettings settings;
};

ContactShadowView contactShadowView(const Mat4& viewProjection, const Mat4& inverseViewProjection, RenderSize size, const Vec3& toLight,
                                    const ContactShadowSettings& settings);

/*
* The mask at a pixel (its centre, from the top-left corner), noise its start's turn: 1 lit, down to 0 where a step is
* blocked, the blocked step's darkness faded by the screen's edge where it read the depth and by the pixel's distance.
* The sky is lit; a march stops at the screen's edge or nearer the camera than CONTACT_NEAREST_W. contact_shadows.comp's
* loop, the depths read as the AO pass reads them. solidAt (the depth prepass's solid mask, read the same way; every
* pixel solid without it) is below a half where a cut-out is nearest: a step landing there is passed over. A cut-out's
* wire is a millimetre thin but would count as a step deep, so a fence's foot went dark where its shadow map already
* draws its holes.
*/
float contactShadowAt(const ContactShadowView& view, const AoDepthImage& depthAt, Vec2 pixel, float noise, const AoDepthImage& solidAt = {});

/*
* A point or spot light a march heads for: its position measured from the camera, its range, its beam and the cosine
* of its outer cone (CONTACT_NO_CONE for a point light) and how far short of it the march stops (contactLightStop).
*/
struct ContactLight {
    Vec3 position;
    float range = 0.0f;
    Vec3 beam{0.0f, 0.0f, -1.0f};
    float cosOuter = static_cast<float>(CONTACT_NO_CONE);
    float stop = 0.0f;
};

// How far short of a light a march stops: its glowing part (emitterRadius) and CONTACT_LIGHT_CLEARANCE past it.
float contactLightStop(float emitterRadius);

// A march's unit way towards a light and how long it is.
struct ContactRay {
    Vec3 toLight;
    float length = 0.0f;
};

// The march from start (camera-relative) towards a light: maxLength long, or as far as the light less its stop if that
// is nearer. None out of the light's range, outside its cone, or nearer it than its stop.
std::optional<ContactRay> contactLightRay(const ContactLight& light, const Vec3& start, float maxLength);

// contactShadowAt towards a point or spot light instead of the view's direction: 1 where the light does not reach.
float contactShadowTowards(const ContactShadowView& view, const AoDepthImage& depthAt, Vec2 pixel, float noise, const ContactLight& light,
                           const AoDepthImage& solidAt = {});

enum class ContactLightKind {
    Point,
    Spot
};

// A seated light given a contact shadow channel: its kind, its slot among the frame's lights of that kind, and whose.
struct ContactLightPick {
    ContactLightKind kind = ContactLightKind::Point;
    int slot = 0;
    Entity entity{};
};

/*
* Up to count of the seated lights (light priority's output: slots are their places in these lists) holding a shadow,
* the ones lighting the focus most brightly (lightImportance) first; a light in previous (last frame's picks) counts
* incumbentAdvantage times as bright, so two similar lights do not swap their channels every frame.
*/
std::vector<ContactLightPick> chooseContactLights(const FrameLighting& seated, const Vec3& focus, const std::vector<Entity>& previous, int count,
                                                  float incumbentAdvantage);

// The picks' slots as the frame's data names them (a spotlight CONTACT_SPOT_SLOT_BASE past its slot), -1 for no pick.
std::array<std::int32_t, CONTACT_MAX_LIGHTS> contactLightSlotCodes(const std::vector<ContactLightPick>& picks);

// A pick's march target, its position measured from the camera.
ContactLight contactLightFor(const FrameLighting& seated, const ContactLightPick& pick, const Vec3& cameraPosition);

// What a frame's contact shadows march towards: the sun, and the local lights given channels. The pass runs for either.
struct ContactShadowFrame {
    bool sun = false;
    std::vector<ContactLightPick> lights;

    bool runs() const {
        return sun || !lights.empty();
    }
};

/*
* The pass's numbers (contact_shadows.comp's storage buffer, std430): the matrices, the size and its inverse, the way to
* the sun with the footprint in w, the options (length, steps, a step's reach, the frame's noise turn), the fades, then
* each local light's position and range, beam and cone, and stop, and the light options (x the local lights' count, y
* 1 when the sun marches too).
*/
struct GpuContactShadowPass {
    float viewProjection[16];
    float inverseViewProjection[16];
    float size[4];
    float toLight[4];
    float options[4];
    float fade[4];
    float lightPositionRange[CONTACT_MAX_LIGHTS][4];
    float lightBeam[CONTACT_MAX_LIGHTS][4];
    float lightStop[CONTACT_MAX_LIGHTS];
    float lightOptions[4];
};

static_assert(sizeof(GpuContactShadowPass) == 352, "GpuContactShadowPass must match contact_shadows.comp's Pass");

GpuContactShadowPass packContactShadowPass(const Mat4& viewProjection, const Mat4& inverseViewProjection, RenderSize size, const Vec3& toLight,
                                           const ContactShadowSettings& settings, std::uint32_t frame, std::span<const ContactLight> lights = {},
                                           bool sun = true);
