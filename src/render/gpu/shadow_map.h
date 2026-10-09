#pragma once

#include "math/mat4.h"
#include "math/vec3.h"
#include "render/gpu/uniform_limits.h"
#include "scene/camera.h"
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
* flashlight beam), from 256 to 1024 pixels, and packed into the atlas each frame.
*/

// The atlas: 8192 x 4096 depth texels (128 MB), room for 32 tiles of the largest size or 512 of the smallest.
inline constexpr std::uint32_t shadowAtlasWidth = 8192;
inline constexpr std::uint32_t shadowAtlasHeight = 4096;
inline constexpr std::uint32_t largestShadowTileSize = 1024;
inline constexpr std::uint32_t smallestShadowTileSize = 256;

// How much world one texel should cover where a light's reach ends. Shadows nearer the light come out sharper.
inline constexpr float shadowTexelTarget = 0.04f;

// The most shadowed lights of each kind; the rest still give light, without shadows.
inline constexpr int maxShadowedPointLights = 16;      // 6 tiles each
inline constexpr int maxShadowedSpotLights = 8;        // 1 tile each
inline constexpr int maxShadowedDirectionalLights = 4; // 1 tile each, always the largest size
inline constexpr int maxShadowTiles = maxShadowedPointLights * 6 + maxShadowedSpotLights + maxShadowedDirectionalLights;

// Tile edge length in pixels for a square view of the given half-width (tan of its half-angle) reaching range: enough
// for shadowTexelTarget across the far end of the view, as a power of two between the smallest and largest tile sizes.
std::uint32_t shadowTileSizeFor(float halfWidth, float range);

// Closest distance to a point light or spotlight that can be shadowed.
inline constexpr float shadowNearPlane = 0.1f;

// How far in front of the camera a directional light's shadows reach. Beyond it, everything counts as lit.
inline constexpr float directionalShadowDistance = 30.0f;

// Which cube face sees this direction from a point light: the axis of its largest component, 0..5 in the order
// +X, -X, +Y, -Y, +Z, -Z. The shader picks faces the same way.
int pointShadowFace(const Vec3& fromLight);

// One cube face's view and projection, in the GPU's depth range. farPlane is the light's range, since nothing beyond it is lit.
// The view is widened by a margin of texels, so it depends on the size of the tile it is drawn into.
Mat4 pointShadowFaceMatrix(const Vec3& lightPosition, int face, float farPlane, std::uint32_t tileSize = largestShadowTileSize);

// A spotlight's view down its beam, just wide enough for its outer cone (plus the texel margin).
Mat4 spotShadowMatrix(const Vec3& lightPosition, const Vec3& direction, float outerAngle, float range,
                      std::uint32_t tileSize = largestShadowTileSize);

// A directional light's box around the part of the camera's view within directionalShadowDistance.
Mat4 directionalShadowMatrix(const Vec3& direction, const Camera& camera);

/*
* Mirrors the ShadowData uniform block in triangle.frag (std140, every member a multiple of 16 bytes): which tiles
* belong to which light. Tile numbers are -1 for a light without a shadow. A point light's six faces are six tiles in
* a row from its first one.
*/
struct ShadowUniformData {
    std::int32_t pointTiles[16][4]; // first tile of point light i at [i / 4][i % 4], one per light slot
    std::int32_t spotTiles[2][4];   // tile of spotlight i at [i / 4][i % 4]
    std::int32_t directionalTiles[4];
    float atlasTexel[4];            // xy: one texel's size in atlas coordinates

    // How strong each light slot's shadow is, 0..1, while shadows fade between lights (0 for a slot without one).
    float pointShadowStrengths[16][4];
    float spotShadowStrengths[2][4];
};

static_assert(sizeof(ShadowUniformData) == 608,"ShadowUniformData must match the shader's ShadowData block");
static_assert(sizeof(ShadowUniformData) <= maxUniformBlockBytes, "the shader can only read the first 4 KB of a uniform block");

/*
* One tile as the shader reads it, from the ShadowTiles storage buffer (std430). There can be over a hundred tiles,
* about 10 KB in all, more than a uniform block can hold, which is why they live in a storage buffer.
*/
struct ShadowTileData {
    float matrix[16]; // column-major view-projection, the same one the shadow pass drew with
    float offset[4];  // normal offset against acne: x per unit of distance from the light, y fixed
    float rect[4];    // where the tile is in the atlas, 0..1: xy corner, zw size
};

static_assert(sizeof(ShadowTileData) == 96, "ShadowTileData must match the shader's ShadowTileData struct");

// One shadow view: its view-projection, and the square of the atlas it is drawn into (pixels, top-left corner). Size 0
// means no square: a point light's cube face that sees nothing on screen, which is not drawn and which the shader counts as lit.
struct ShadowTile {
    Mat4 matrix = Mat4::identity();
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t size = largestShadowTileSize;
};

// What the shadow pass draws (one tile per view, in tile order) and what the shader reads (the uniforms, and tileData
// for the storage buffer, in the same order as tiles).
struct ShadowPlan {
    ShadowUniformData uniforms;
    std::vector<ShadowTile> tiles;
    std::vector<ShadowTileData> tileData;
};

// Hand out atlas tiles to the shadow-casting lights, in the same slots packLighting gives the shader. Tiles are numbered
// directional lights first, then spotlights, then six per point light, each in slot order.
ShadowPlan planShadows(const FrameLighting& lighting, const Camera& camera);
