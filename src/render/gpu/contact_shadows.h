#pragma once

#include "core/settings_visitor.h"
#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/ambient_occlusion.h"
#include "render/gpu/contact_shadows_shared.h"
#include "render/gpu/upscaling.h"

#include <cstdint>

/*
* Contact shadows: short shadows the shadow map misses, from the depth image. A shadow map's texels are millimetres to
* centimetres across and its lookups are pushed towards the light against acne, so a small object's shadow comes loose
* from it or vanishes, worst where the sun grazes. After the depth prepass every pixel marches a short way from its
* surface towards the sun, each step projected onto the screen: a step behind the surface the depth image shows there
* (by more than a pixel's rounding, by less than an assumed thickness, since the depth image shows only fronts) is
* blocked, and the pixel is in shadow. The pass writes a mask the main pass multiplies the sun's shadow by; the shadow
* map stays in charge of everything longer. Only what is on screen casts one, so the marches are short and fade at the
* screen's edge and with distance.
*
* The march's start turns with blue noise per pixel and the frame, so anti-aliasing averages the steps.
*/

struct ContactShadowSettings {
    // On by default (off on the Low preset).
    bool enabled = true;

    // How far each pixel marches towards the sun (metres) and in how many steps.
    float length = 0.3f;
    int steps = 16;

    // How thick anything on screen is taken to be (metres): a step further behind a surface than this passes behind it.
    // Too thin and thin objects are missed; too thick and objects grow dark haloes behind them: at 0.2 m a fence post
    // cast a second, wider stub of shadow beside its own (the user's "double shadow"). 5 cm is still over twice the
    // march's step (0.3 m in 16), so a step cannot jump past a solid object's front.
    float thickness = 0.05f;

    // The shadow fades out between these view distances (metres), where a pixel covers too much for so short a march.
    float fadeStart = 40.0f;
    float fadeEnd = 60.0f;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(ContactShadowSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a length or thickness not above 0, steps outside 1..CONTACT_MAX_STEPS, or a fade
// that ends before it starts (or starts below 0).
void validateContactShadows(const ContactShadowSettings& settings);

// Step number step of steps, turned by noise (0..1): its distance along the march, (step + noise) / steps of length.
float contactStepDistance(int step, int steps, float noise, float length);

// The bias a point must be behind the depth image's surface by to count: CONTACT_BIAS_PIXELS pixels' footprint at
// that view distance (footprint: a pixel's height per metre of view distance, pixelFootprint).
float contactBias(float footprint, float viewDistance);

// Whether a step at rayDistance (along the view) is blocked by the surface the depth image shows at sceneDistance:
// behind it by more than bias and less than thickness.
bool contactBlocked(float rayDistance, float sceneDistance, float bias, float thickness);

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
* wire is a millimetre thin but would count as thickness deep, so a fence's foot went dark where its shadow map already
* draws its holes.
*/
float contactShadowAt(const ContactShadowView& view, const AoDepthImage& depthAt, Vec2 pixel, float noise, const AoDepthImage& solidAt = {});

/*
* The pass's numbers (contact_shadows.comp's storage buffer, std430): the matrices, the size and its inverse, the way to
* the light with the footprint in w, the options (length, steps, thickness, the frame's noise turn) and the fades.
*/
struct GpuContactShadowPass {
    float viewProjection[16];
    float inverseViewProjection[16];
    float size[4];
    float toLight[4];
    float options[4];
    float fade[4];
};

static_assert(sizeof(GpuContactShadowPass) == 192, "GpuContactShadowPass must match contact_shadows.comp's Pass");

GpuContactShadowPass packContactShadowPass(const Mat4& viewProjection, const Mat4& inverseViewProjection, RenderSize size, const Vec3& toLight,
                                           const ContactShadowSettings& settings, std::uint32_t frame);
