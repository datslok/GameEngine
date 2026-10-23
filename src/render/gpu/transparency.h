#pragma once

#include "core/settings_visitor.h"
#include "math/vec3.h"
#include "render/gpu/transparency_shared.h"
#include "render/gpu/upscaling.h"
#include "scene/asset_handles.h"
#include "scene/frame_description.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/*
* Transparent surfaces: what glTF blends (alphaMode BLEND) and thin glass (KHR_materials_transmission), drawn after the
* opaque scene into its picture with dual-source blending, picture = added + transmittance x picture (per channel), so
* coloured glass tints what is behind it. Mirrored by assets/shaders/transparency.glsl and checked against it on the GPU.
*
* Two different things make a surface seen through:
* - coverage (glTF's alpha): that share of the pixel is the surface, all of it (its reflection too), the rest a hole;
* - transmission (glass): the whole pixel is the surface; its reflection stays whole (Fresnel: about 4% head-on, nearly
*   all at grazing angles), and of the rest the share transmission passes through, tinted by the base colour, in
*   place of the light the surface would scatter (its diffuse part). Metals pass none.
*/

// What a transparent fragment gives: the light it adds, and the share of what is behind that still shows (per channel).
struct TransparentLight {
    Vec3 added;
    Vec3 transmittance;
};

/*
* The fragment's light as shaded: the diffuse part (direct and from all around, already times the diffuse colour), the
* specular part (its reflection), its glow; its base colour and the specular albedo (how much of the light its surface
* reflects in all, from the BRDF table). alpha is coverage, transmission and metallic shares (0..1).
*/
TransparentLight transparentBlend(float alpha, float transmission, float metallic, const Vec3& diffuseLight, const Vec3& specularLight,
                                  const Vec3& emissive, const Vec3& baseColour, const Vec3& specularAlbedo);

// The share of what is behind that still shows (transparentBlend's transmittance): one function for the blend over the
// picture and the light crossing glass on its way to a surface (glassLightTransmittance), so the two cannot disagree.
Vec3 transparentTransmittance(float alpha, float transmission, float metallic, const Vec3& baseColour, const Vec3& specularAlbedo);

/*
* Tinted shadows: the light crossing one pane at an angle whose cosine to its normal is cosine (either side): what the
* glass lets through, the reflected share being Schlick's Fresnel at that angle (a beam meets a smooth surface at one
* angle, where the view's blend uses the specular albedo, its average over the rough lobe). Metals pass nothing.
*/
Vec3 glassLightTransmittance(float alpha, float transmission, float metallic, const Vec3& baseColour, float cosine);

/*
* Whether a pane a ray towards a light meets counts: a face turned towards the light (the ray, running from the lit point
* to the light, meets it from behind), as the camera draws only faces turned towards it, so a pane modelled as a thin
* box tints the light once, as it tints the view once; a double-sided surface counts from both sides.
*/
bool glassFaceCounts(bool rayMeetsFront, bool doubleSided);

// Whether the segment from a point along a unit direction for length passes within radius of centre.
bool segmentMeetsSphere(const Vec3& from, const Vec3& direction, float length, const Vec3& centre, float radius);

// The chosen draws (indices into draws) that are opaque, and those that are transparent (isTransparent), in the order chosen.
std::vector<std::size_t> opaqueDraws(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen);
std::vector<std::size_t> transparentDraws(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen);

// One draw of one transparent object: its mesh, its entry in the draw list, and whether it draws its back faces only
// (then its front ones in the next draw) or its front faces.
struct TransparentBatch {
    MeshHandle mesh;
    std::uint32_t firstInstance = 0;
    bool backFaces = false;
};

// The transparent draws in drawing order: order lists the draw indices as their draw list entries (from firstInstance).
struct SortedTransparentDraws {
    std::vector<std::size_t> order;
    std::vector<TransparentBatch> batches;
};

/*
* Blending adds layers in order, so the chosen transparent draws are drawn far to near, by the distance from camera to
* the centre of their bounds (their model's origin without bounds), equal distances in the order chosen. Sorting whole
* objects is exact while they do not overlap in depth. A double-sided object is drawn twice from its one entry, its back
* faces then its front ones, so its far side shows through its near side; a one-sided one shows only its front.
*/
SortedTransparentDraws sortTransparentDraws(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen, const Vec3& camera,
                                            std::uint32_t firstInstance);

/*
* The glass pass's fragment push constants, after triangle.vert's 64 bytes (scene_surface.glsl's TransparentView): x 1
* while it reads the reflection probe round the camera (else the light probes' or flat ambient's mirror light), y the
* probe's prefiltered levels.
*/
struct GpuTransparentView {
    float probe[4];
};

static_assert(sizeof(GpuTransparentView) == 16, "GpuTransparentView must match scene_surface.glsl's TransparentView");

// Where GpuTransparentView starts in the glass pass's push constants: after the vertex shader's capture matrix.
inline constexpr std::uint32_t transparentViewOffset = 64;

GpuTransparentView packTransparentView(bool readsProbe, std::uint32_t probeLevels);

/*
* The reactive mask. Anti-aliasing (TAA, FSR, DLSS) rebuilds each pixel from past frames, finding where it was by the
* motion of what the depth image holds; glass is not in it, so a glass pixel moves as the wall behind it does, and
* moving glass, or a reflection sliding over it, smears. The mask tells them how far to distrust the history at each
* pixel: the share of its light the transparent surfaces changed, measured from the picture before them and after.
* FSR takes it as its reactive mask, DLSS as its bias towards the current colour, the engine's own resolve as a larger
* share of the new frame (taaReactiveShare).
*/
struct TransparencySettings {
    // Glass tints the light passing through it on its way to a surface (the main pass's shadowed lights, the probes' and
    // reflections' rays), by rays that see only glass. On GPUs with ray queries; ignored elsewhere.
    bool glassShadows = true;

    // Off leaves the history trusted everywhere, as before the mask, for comparing.
    bool reactiveMask = true;

    // Strengthens or weakens the measured share (1 as measured), for judging the trade: more stops smears sooner, but
    // leaves glass as unsmoothed as one frame.
    float reactiveScale = 1.0f;

    // The most the mask says: just under 1 keeps a little history even where glass makes the whole pixel, so its edges
    // stay smoothed.
    float reactiveLimit = 0.9f;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(TransparencySettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a negative scale or a limit outside 0..1.
void validateTransparency(const TransparencySettings& settings);

/*
* One pixel's mask: in each channel the change the transparent surfaces made, as a share of the brighter of before and
* after (a share, so the same at any exposure, which is not known yet when it is measured), the most changed channel,
* times the scale, at most the limit. Negative light counts as none.
*/
float reactiveShare(const Vec3& opaque, const Vec3& blended, float scale, float limit);

// The mask pass's push constants (reactive_mask.comp): the render screen it walks (width, height), the settings.
struct GpuReactivePass {
    float size[4];
    float options[4]; // x: the scale, y: the limit
};

static_assert(sizeof(GpuReactivePass) == 32, "GpuReactivePass must match reactive_mask.comp's push constants");

GpuReactivePass packReactivePass(RenderSize render, const TransparencySettings& settings);

/*
* The glass a ray towards a light is tested against before it is traced (scene set 1, binding 26): most pixels see a
* light past no glass at all, and a sphere test is far cheaper than a ray. counts: x the spheres held, y 1 when every
* ray must be traced (a glass draw without bounds, or more glass than the buffer holds). Spheres: world centre, radius.
*/
inline constexpr std::size_t glassBoundsLimit = TRANSPARENCY_GLASS_BOUNDS_LIMIT;

struct GpuGlassBounds {
    std::uint32_t counts[4];
    float spheres[TRANSPARENCY_GLASS_BOUNDS_LIMIT][4];
};

static_assert(sizeof(GpuGlassBounds) == 16 + 16 * TRANSPARENCY_GLASS_BOUNDS_LIMIT, "GpuGlassBounds must match ray_scene.glsl's GlassBounds");

// Every transparent draw's bounds, on screen or not (a window behind the camera still shades the floor in front of it);
// none when disabled.
GpuGlassBounds packGlassBounds(const std::vector<DrawItem>& draws, bool enabled);

// Whether a segment may meet glass by the bounds, as the shaders test it before tracing.
bool glassMayLieBetween(const GpuGlassBounds& bounds, const Vec3& from, const Vec3& direction, float length);
