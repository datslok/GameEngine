#pragma once

#include "math/mat4.h"
#include "math/vec3.h"
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
* All views share one depth texture, an atlas of equal square tiles, so the shadow pass is a single render pass and
* the shader binds one texture.
*/

inline constexpr std::uint32_t shadowTileSize = 1024; // Pixels along each side of a tile.
inline constexpr std::uint32_t shadowAtlasColumns = 8;
inline constexpr std::uint32_t shadowAtlasRows = 4;
inline constexpr int shadowTileCount = 32;

// The atlas has room for this many shadowed lights of each kind; the rest still give light, without shadows.
inline constexpr int maxShadowedPointLights = 4;      // 6 tiles each
inline constexpr int maxShadowedSpotLights = 4;       // 1 tile each
inline constexpr int maxShadowedDirectionalLights = 4; // 1 tile each

// Closest distance to a point light or spotlight that can be shadowed.
inline constexpr float shadowNearPlane = 0.1f;

// How far in front of the camera a directional light's shadows reach. Beyond it, everything counts as lit.
inline constexpr float directionalShadowDistance = 30.0f;

// Which cube face sees this direction from a point light: the axis of its largest component, 0..5 in the order
// +X, -X, +Y, -Y, +Z, -Z. The shader picks faces the same way.
int pointShadowFace(const Vec3& fromLight);

// One cube face's view and projection, in the GPU's depth range. farPlane is the light's range, since nothing beyond it is lit.
Mat4 pointShadowFaceMatrix(const Vec3& lightPosition, int face, float farPlane);

// A spotlight's view down its beam, just wide enough for its outer cone.
Mat4 spotShadowMatrix(const Vec3& lightPosition, const Vec3& direction, float outerAngle, float range);

// A directional light's box around the part of the camera's view within directionalShadowDistance.
Mat4 directionalShadowMatrix(const Vec3& direction, const Camera& camera);

/*
* Mirrors the ShadowData block in triangle.frag (std140, every member a multiple of 16 bytes).
* Tile numbers are -1 for a light without a shadow. A point light's six faces are six tiles in a row from its first one.
*/
struct ShadowUniformData {
    float tileMatrices[shadowTileCount][16]; // column-major view-projection per tile
    float tileOffsets[shadowTileCount][4];    // normal offset against acne: x per unit of distance from the light, y fixed
    std::int32_t pointTiles[4][4];           // first tile of point light i at [i / 4][i % 4]
    std::int32_t spotTiles[4];
    std::int32_t directionalTiles[4];
    float atlasTexel[4];                     // xy: one texel's size in atlas coordinates
};

static_assert(sizeof(ShadowUniformData) == 2672, "ShadowUniformData must match the shader's ShadowData block");

// What the shadow pass draws (one matrix per used tile, in tile order) and what the shader reads.
struct ShadowPlan {
    ShadowUniformData uniforms;
    std::vector<Mat4> tileMatrices;
};

// Hand out atlas tiles to the shadow-casting lights, in the same order and slots packLighting gives the shader.
ShadowPlan planShadows(const FrameLighting& lighting, const Camera& camera);
