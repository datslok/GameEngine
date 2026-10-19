#pragma once

#include "core/settings_visitor.h"
#include "math/mat4.h"
#include "math/vec3.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/shadow_atlas.h"
#include "scene/camera.h"
#include "scene/frame_description.h"
#include "scene/lighting.h"

#include <cstdint>
#include <vector>

/*
* Shadow maps for every kind of light. A shadow map is the scene's depth seen from a light: a camera at the light
* records how far the nearest surface is in each direction, and a point further than that is in shadow.
* - A point light shines every way, so it needs six views, one per face of a cube around it (+X, -X, +Y, -Y, +Z, -Z).
* - A spotlight needs one perspective view down its beam.
* - A directional light (parallel rays) needs one box-shaped (orthographic) view, around what the camera sees nearby.
* All views share one depth texture, an atlas of square tiles, so the shadow pass is a single render pass and the
* shader binds one texture. Tiles are sized by how far their light reaches (a lamp needs far fewer texels than a long
* flashlight beam), from 256 to 1024 pixels (2048 for spotlights), and packed into the atlas each frame.
*/

// The atlas: 8192 x 4096 depth texels (128 MB), room for 8 tiles of the largest size, 32 of the standard size or 512 of
// the smallest. Directional lights always get the standard size and point light faces at most that; only spotlights,
// few and usually still (so their cached tiles cost nothing to keep), may grow to the largest.
inline constexpr std::uint32_t shadowAtlasWidth = 8192;
inline constexpr std::uint32_t shadowAtlasHeight = 4096;
inline constexpr std::uint32_t largestShadowTileSize = 2048;
inline constexpr std::uint32_t standardShadowTileSize = 1024;
inline constexpr std::uint32_t smallestShadowTileSize = 256;

// The tunable part of shadows: read every frame, so a change shows on the next frame (the tiles it affects are redrawn).
struct ShadowSettings {
    // How much world one texel should cover where a point light's reach ends. Shadows nearer the light come out sharper.
    float texelTarget = 0.04f;

    // The same for spotlights, finer: their beams usually land on the ground at a slant well short of their reach, which
    // stretches each texel along the ground, and the floodlights' step and pole shadows came out jagged at 0.04.
    float spotTexelTarget = 0.02f;

    // How far in front of the camera a directional light's shadows reach. Beyond it, everything counts as lit.
    float directionalDistance = 30.0f;

    // How far behind a directional light's box (towards the light) objects still cast shadows into it.
    float directionalCasterReach = 50.0f;

    // Shadow acne fix: points are moved off their surface along the normal by this many texels. It is the only bias (the
    // shadow pass has no hardware depth bias), so it is a little larger than usual: 1.5 left faint stripes on faces the
    // light grazes. Measured in texels, it shrinks and grows with each tile's size.
    float normalOffsetTexels = 2.5f;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(ShadowSettings& settings, SettingsVisitor& visitor);

// The most shadowed lights of each kind; the rest still give light, without shadows.
inline constexpr int maxShadowedPointLights = 64;      // 6 tiles each; cached tiles cost little, but each shadowed pixel pays 32 samples per light
inline constexpr int maxShadowedSpotLights = 8;        // 1 tile each
inline constexpr int maxShadowedDirectionalLights = 4; // 1 tile each, always the standard size
inline constexpr int maxShadowTiles = maxShadowedPointLights * 6 + maxShadowedSpotLights + maxShadowedDirectionalLights;

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

// A directional light's box around the part of the camera's view within the settings' directional distance.
Mat4 directionalShadowMatrix(const Vec3& direction, const Camera& camera, const ShadowSettings& settings = {});

/*
* One light's shadow as the plan hands it out: its first tile (-1 for none; a point light's six faces are six tiles in a
* row from it), how strong it is while shadows fade between lights (0..1), and the glowing size that softens it.
* attachShadows copies these into the light records the shader reads.
*/
struct LightShadow {
    std::int32_t firstTile = -1;
    float strength = 0.0f;
    float emitterRadius = 0.0f;
};

/*
* One tile as the shader reads it, from the ShadowTiles storage buffer (std430). There can be up to maxShadowTiles (396) of
* them, about 44 KB in all, far more than a uniform block can hold, which is why they live in a storage buffer.
*/
struct ShadowTileData {
    float matrix[16]; // column-major view-projection: the shadow pass's view, for points measured from the camera
    float offset[4];  // normal offset against acne: x per unit of distance from the light, y fixed
    float rect[4];    // where the tile is in the atlas, 0..1: xy corner, zw size

    // For soft shadows, perspective views only (zeros for a directional box): x near and y far plane, to turn stored
    // depths back into distances, and z the world size of one texel per unit of distance from the light.
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
// to frame, so lights keep their squares of the atlas and what was drawn into them can be kept.
ShadowPlan planShadows(const FrameLighting& lighting, const Camera& camera, ShadowAtlasLayout& layout,
                       const ShadowSettings& settings = {});

// The same, packed into an empty atlas (for one-off plans and tests).
ShadowPlan planShadows(const FrameLighting& lighting, const Camera& camera, const ShadowSettings& settings = {});

// Give each light record the shadow the plan handed out for its slot (the plan and the lights from the same frame's lights).
void attachShadows(GpuLights& lights, const ShadowPlan& plan);
