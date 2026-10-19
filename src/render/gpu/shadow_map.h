#pragma once

#include "core/settings_visitor.h"
#include "math/mat4.h"
#include "math/vec3.h"
#include "render/gpu/cascades.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/shadow_atlas.h"
#include "scene/camera.h"
#include "scene/frame_description.h"
#include "scene/lighting.h"

#include <cstdint>
#include <limits>
#include <vector>

/*
* Shadow maps for every kind of light. A shadow map is the scene's depth seen from a light: a camera at the light
* records how far the nearest surface is in each direction, and a point further than that is in shadow.
* - A point light shines every way, so it needs six views, one per face of a cube around it (+X, -X, +Y, -Y, +Z, -Z).
* - A spotlight needs one perspective view down its beam.
* - A directional light (parallel rays, the sun) needs box-shaped (orthographic) views, one per cascade: slices of what
*   the camera sees, near ones sharp and far ones coarse (cascades.h).
* All views share one depth texture, an atlas of square tiles, so the shadow pass is a single render pass and the
* shader binds one texture. Tiles are sized by how far their light reaches (a lamp needs far fewer texels than a long
* flashlight beam), from 256 to 1024 pixels (2048 for spotlights), and packed into the atlas each frame.
*/

// How sharp the sun's shadows are: the size of each cascade's tile. 4096 (the default, Ultra) halves every texel against
// 2048 (High) but takes 256 MB for the four, so the atlas grows from 128 to 384 MB to hold them.
enum class CascadeResolution {
    Pixels1024,
    Pixels2048,
    Pixels4096
};

// A cascade's tile edge in pixels.
std::uint32_t cascadeTilePixels(CascadeResolution resolution);

// How a sun's shadow passes from one cascade to the next inside the blend band near a cascade's edge.
enum class CascadeTransition {
    Blend, // both cascades, mixed: smooth, but those pixels look the shadow up twice
    Dither // each pixel takes one, the next more often nearer the edge: one lookup, grainy until temporal anti-aliasing smooths it
};

// The tunable part of shadows: read every frame, so a change shows on the next frame (the tiles it affects are redrawn).
struct ShadowSettings {
    // How much world one texel should cover where a point light's reach ends. Shadows nearer the light come out sharper.
    float texelTarget = 0.04f;

    // The same for spotlights, finer: their beams usually land on the ground at a slant well short of their reach, which
    // stretches each texel along the ground, and the floodlights' step and pole shadows came out jagged at 0.04.
    float spotTexelTarget = 0.02f;

    // How far along the view the sun's shadows reach (at most the camera's far plane). Beyond it, everything counts as
    // lit; the last cascade fades out over its blend band before that.
    float directionalDistance = 200.0f;

    // How far behind each cascade's box (towards the light) objects still cast shadows into it. A box's depth costs no
    // texels, only a few more casters to draw, and a low sun throws a tall tower's shadow a long way.
    float directionalCasterReach = 200.0f;

    // End the cascades at the farthest surface on screen, measured from the depth image, instead of always at
    // directionalDistance (sample distribution shadow maps, cascades.h): in a room, every cascade gets far sharper.
    bool fitCascadesToDepth = true;

    // How many slices the sun's shadow is cut into (1 to maxShadowCascades): more give sharper shadows near the camera
    // for the same distance, each costing a drawing of what it covers.
    int cascadeCount = maxShadowCascades;

    // How the slices are spaced: 0 evenly, 1 logarithmically (each the same multiple further than the last), in between
    // a blend (see cascadeSplits). Leaning logarithmic keeps the first cascade a few metres deep, so shadows up close
    // are sharp (0.75 left it 13 m deep, with 3.6 cm texels); the last cascade's size depends only on the shadow
    // distance, so only the middle ones grow coarser.
    float cascadeSplitLambda = 0.95f;

    // How wide the band near a cascade's edge is where it hands over to the next, as a share of its radius.
    float cascadeBlendBand = 0.1f;

    // How the handover looks (see CascadeTransition).
    CascadeTransition cascadeTransition = CascadeTransition::Blend;

    // Each cascade's tile size (see CascadeResolution); 4096 by default (Ultra), 2048 on High. The atlas is remade when
    // it changes, and every shadow redrawn.
    CascadeResolution cascadeResolution = CascadeResolution::Pixels4096;

    // Shadow acne fix: points are moved off their surface along the normal by this many texels. It is the only bias (the
    // shadow pass has no hardware depth bias), so it is a little larger than usual: 1.5 left faint stripes on faces the
    // light grazes. Measured in texels, it shrinks and grows with each tile's size. For the sun it is how far points move
    // towards the sun instead (more on slopes, see sunShadowBias), so cascades agree on where shadow edges are.
    float normalOffsetTexels = 2.5f;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(ShadowSettings& settings, SettingsVisitor& visitor);

// The atlas the settings need: the usual one, or with Ultra cascades one grown by exactly their extra room.
ShadowAtlasSize shadowAtlasSizeFor(const ShadowSettings& settings);

// The most shadowed lights of each kind; the rest still give light, without shadows.
inline constexpr int maxShadowedPointLights = 64;      // 6 tiles each; cached tiles cost little, but each shadowed pixel pays 32 samples per light
inline constexpr int maxShadowedSpotLights = 8;        // 1 tile each
inline constexpr int maxShadowedDirectionalLights = 1; // the sun: up to maxShadowCascades tiles of cascadeTileSize
inline constexpr int maxShadowTiles = maxShadowedPointLights * 6 + maxShadowedSpotLights + maxShadowedDirectionalLights * maxShadowCascades;

// Tile edge length in pixels for a square view of the given half-width (tan of its half-angle) reaching range: enough
// for one texel to cover texelTarget across the far end of the view, as a power of two from the smallest tile size up
// to largestSize.
std::uint32_t shadowTileSizeFor(float halfWidth, float range, float texelTarget, std::uint32_t largestSize);

// Closest distance to a point light or spotlight that can be shadowed.
inline constexpr float shadowNearPlane = 0.1f;

// Which cube face sees this direction from a point light: the axis of its largest component, 0..5 in the order
// +X, -X, +Y, -Y, +Z, -Z. The shader picks faces the same way.
int pointShadowFace(const Vec3& fromLight);

// One cube face's view and projection, in the GPU's depth range. farPlane is the light's range, since nothing beyond it is lit.
// The view is widened by a margin of texels, so it depends on the size of the tile it is drawn into.
Mat4 pointShadowFaceMatrix(const Vec3& lightPosition, int face, float farPlane, std::uint32_t tileSize = standardShadowTileSize);

// A spotlight's view down its beam, just wide enough for its outer cone (plus the texel margin).
Mat4 spotShadowMatrix(const Vec3& lightPosition, const Vec3& direction, float outerAngle, float range,
                      std::uint32_t tileSize = standardShadowTileSize);

/*
* One light's shadow as the plan hands it out: its first tile (-1 for none; a point light's six faces are six tiles in a
* row from it, and so are a sun's cascades, cascades of them), how strong it is while shadows fade between lights (0..1),
* and the glowing size that softens it (for a sun, tan of its angular radius). attachShadows copies these into the light
* records the shader reads.
*/
struct LightShadow {
    std::int32_t firstTile = -1;
    float strength = 0.0f;
    float emitterRadius = 0.0f;
    std::int32_t cascades = 0;
};

/*
* One tile as the shader reads it, from the ShadowTiles storage buffer (std430). There can be up to maxShadowTiles (396) of
* them, about 44 KB in all, far more than a uniform block can hold, which is why they live in a storage buffer.
*/
struct ShadowTileData {
    float matrix[16]; // column-major view-projection: the shadow pass's view, for points measured from the camera
    float offset[4];  // against acne: x normal offset per unit of distance from the light, y fixed (for a sun cascade,
                      // its base bias towards the sun, see sunShadowBias)
    float rect[4];    // where the tile is in the atlas, 0..1: xy corner, zw size

    // For soft shadows. Perspective views: x near and y far plane, to turn stored depths back into distances, and z the
    // world size of one texel per unit of distance from the light. A sun's cascade boxes: x 0 (no near plane, which tells
    // them apart), y the box's depth, z one texel's world size (the same everywhere in a box) and w its radius.
    float lens[4];
};

static_assert(sizeof(ShadowTileData) == 112, "ShadowTileData must match the shader's ShadowTileData struct");

// One shadow view: its view-projection, and the square of the atlas it is drawn into (pixels, top-left corner). Size 0
// means no square: a point light's cube face that sees nothing on screen, which is not drawn and which the shader counts as lit.
// reach is the sphere its light lights (centre at the light, the range as radius; negative for a directional light,
// which reaches everywhere): nothing wholly outside it can block the light where it shines, so the shadow pass skips
// those casters (a soft shadow's wide filter could reach them only where the light has faded to almost nothing).
struct ShadowTile {
    Mat4 matrix = Mat4::identity();
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t size = standardShadowTileSize;
    BoundingSphere reach;
};

// What the shadow pass draws (one tile per view, in tile order) and what the shader reads: each light's shadow, one record
// per light the shader gets in slot order (as packLighting gives them), and tileData for the storage buffer, in the same
// order as tiles.
struct ShadowPlan {
    std::vector<LightShadow> directional;
    std::vector<LightShadow> spots;
    std::vector<LightShadow> points;
    std::vector<ShadowTile> tiles;
    std::vector<ShadowTileData> tileData;
};

// Hand out atlas tiles to the shadow-casting lights, in the same slots packLighting gives the shader. Tiles are numbered
// directional lights first, then spotlights, then six per point light, each in slot order. The layout lasts from frame
// to frame, so lights keep their squares of the atlas and what was drawn into them can be kept. The layout must be for the
// atlas the settings need (shadowAtlasSizeFor), or this throws std::invalid_argument. The sun's cascades end at the
// shadow distance, the camera's far plane or fittedSunDistance (fitted to the screen's depths, fittedShadowDistance in
// cascades.h), whichever is nearest.
ShadowPlan planShadows(const FrameLighting& lighting, const Camera& camera, ShadowAtlasLayout& layout,
                       const ShadowSettings& settings = {}, float fittedSunDistance = std::numeric_limits<float>::infinity());

// The same, packed into an empty atlas (for one-off plans and tests).
ShadowPlan planShadows(const FrameLighting& lighting, const Camera& camera, const ShadowSettings& settings = {},
                       float fittedSunDistance = std::numeric_limits<float>::infinity());

// Give each light record the shadow the plan handed out for its slot (the plan and the lights from the same frame's lights).
void attachShadows(GpuLights& lights, const ShadowPlan& plan);
