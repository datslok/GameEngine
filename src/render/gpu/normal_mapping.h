#pragma once

#include "math/mat4.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "render/gpu/normal_mapping_shared.h"

/*
* Normal mapping, mirrored from triangle.vert and triangle.frag so it can be tested: a texture of small tilts applied to
* the interpolated normal, written in the frame of the surface (tangent: +u, bitangent: up the picture, normal: out).
*/

/*
* A normal map texel (0..1 per channel, read as linear numbers) as a direction in that frame: c * 2 - 1, with the
* material's scale on the sideways parts (glTF's normalTexture.scale). Scale 0 gives (0, 0, z) for any texel.
*/
Vec3 decodeNormalTexel(const Vec3& texel, float scale);

/*
* The shading normal: the frame's axes weighted by the decoded direction, then normalised. The bitangent is
* tangent.w * cross(normal, tangent). The interpolated normal and tangent are used as they arrive, not normalised first,
* as MikkTSpace asks so that the result matches the baker's. Zero when there is no usable direction.
*/
Vec3 perturbNormal(const Vec3& normal, const Vec4& tangent, const Vec3& tangentSpaceNormal);

/*
* Normal map filtering (Toksvig 2005): the material's roughness widened by the bumps a smaller mip averaged away. A mip
* texel is the average of the unit directions under it, shorter than 1 the more they disagree; its spread (1 - L) / L
* adds twice over to alpha squared (alpha = roughness squared), the scale's square stretching it like the slopes. The
* texel is 0..1 as sampled; shortening within 8-bit rounding (normal_mapping_shared.h) counts as none. At most 1.
*/
float toksvigRoughness(float roughness, const Vec3& texel, float scale);

/*
* A vertex tangent carried into the world: the model matrix moves it (it lies in the surface, like an edge), normalised
* like the vertex normal, and its sign multiplied by the matrix's handedness so a mirrored object's bumps stay bumps.
*/
Vec4 transformTangent(const Mat4& model, const Vec4& tangent);
