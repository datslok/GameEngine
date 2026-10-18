#include "render/gpu/normal_mapping.h"

#include <cmath>

Vec3 decodeNormalTexel(const Vec3& texel, float scale) {
    return Vec3{
        (texel.x * 2.0f - 1.0f) * scale,
        (texel.y * 2.0f - 1.0f) * scale,
        texel.z * 2.0f - 1.0f
    };
}

/*
* A change of basis: the texel's components measured along tangent, bitangent and normal. Normalising the decoded
* direction first would change only the length, which the final normalisation removes anyway.
*/
Vec3 perturbNormal(const Vec3& normal, const Vec4& tangent, const Vec3& tangentSpaceNormal) {
    const Vec3 tangentDirection{tangent.x, tangent.y, tangent.z};
    const Vec3 bitangent = normal.cross(tangentDirection) * tangent.w;
    const Vec3 combined = tangentDirection * tangentSpaceNormal.x + bitangent * tangentSpaceNormal.y + normal * tangentSpaceNormal.z;
    const float length = combined.length();

    if (!(length > 0.0f) || !std::isfinite(length)) {
        return Vec3{};
    }

    return combined * (1.0f / length);
}

Vec4 transformTangent(const Mat4& model, const Vec4& tangent) {
    const Vec3 moved = transformDirection(model, Vec3{tangent.x, tangent.y, tangent.z});
    const float length = moved.length();
    const Vec3 direction = length > 0.0f ? moved * (1.0f / length) : Vec3{};
    return Vec4{direction.x, direction.y, direction.z, tangent.w * handedness(model)};
}
