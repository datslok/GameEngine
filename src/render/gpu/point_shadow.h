#pragma once

#include "math/mat4.h"
#include "math/vec3.h"
#include "render/gpu/light_uniforms.h"

#include <cstdint>

/*
* Shadows for a point light. A point light shines every way, so its shadow map is six square depth images, one per face of
* a cube around the light: six cameras at the light, each looking along one axis (+X, -X, +Y, -Y, +Z, -Z).
* The six images share one depth texture (an atlas), three faces across and two down, so the shadow pass is a single
* render pass and the shader binds one texture.
*/

// Pixels along each side of one face.
inline constexpr std::uint32_t pointShadowFaceSize = 2048;
inline constexpr std::uint32_t pointShadowAtlasColumns = 3;
inline constexpr std::uint32_t pointShadowAtlasRows = 2;

// Closest distance to the light that can be shadowed.
inline constexpr float pointShadowNearPlane = 0.1f;

// Which face sees this direction from the light: the axis of its largest component, 0..5 in the order +X, -X, +Y, -Y, +Z, -Z.
// The shader picks faces the same way.
int pointShadowFace(const Vec3& fromLight);

// One face's view and projection, in the GPU's depth range. farPlane is the light's range, since nothing beyond it is lit.
Mat4 pointShadowFaceMatrix(const Vec3& lightPosition, int face, float farPlane);

/*
* Mirrors the ShadowData block in triangle.frag (std140): the six face matrices, column-major, then the settings.
*/
struct ShadowUniformData {
    float faceMatrices[6][16];
    float settings[4]; // x: normal offset per unit of distance from the light, y: atlas texel width, z: atlas texel height
};

static_assert(sizeof(ShadowUniformData) == 400, "ShadowUniformData must match the shader's ShadowData block");

// Shadow data for the point light packed into slot lights.counts[3]. With no shadowed light (-1) the shader ignores it.
ShadowUniformData packPointShadow(const LightUniformData& lights);
