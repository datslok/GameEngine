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
